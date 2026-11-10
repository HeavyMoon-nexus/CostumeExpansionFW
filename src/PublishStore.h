#pragma once

#include "SkinRebind.h"
#include "BoxStore.h"  // EnchantEffectInfo (the frozen fallback's type)

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace RE
{
    class TESObjectARMO;
}

namespace CostumeFW
{
    struct PubEnchantEffect
    {
        std::string mgef;
        float magnitude{ 0.0f };
    };

    struct PubSnapshot
    {
        int pubSlot{ -1 };
        std::string label;
        // The logical box this costume IS. Publishing does not destroy the box,
        // it moves it into a frozen state, so the id survives the round trip and
        // the box that comes back out of unpublish is the same box (PLAN §2.4).
        std::string boxId;
        // The box token held in reserve for it, canonical colon-id. Unpublish
        // returns to THIS token or to none; FreeTokens will not hand it out while
        // the costume is published (PLAN §5.3/§5.4).
        std::string sourceToken;
        // The biped slot the costume was published from. Display, the published
        // token's biped mask, and the one-time migration that fills sourceToken
        // for a snapshot written before 1.6.4 - never identity.
        int sourceSlot{ 0 };
        int armorType{ 0 };
        std::string manualAbility;
        int rev{ 0 };
        std::vector<std::string> contents;
        std::unordered_map<std::string, std::shared_ptr<const ContentSettings>> settings;
        std::unordered_map<std::string, std::vector<PubEnchantEffect>> enchants;
    };

    struct PubSaveState
    {
        std::uint8_t pubSlot{ 0 };
        bool hidden{ false };
    };

    struct PubBindSave
    {
        std::uint8_t pubSlot{ 0 };
        RE::FormID actorFormID{ 0 };
        std::uint8_t flags{ 0 };
    };
    struct PubBindingInfo
    {
        int pubSlot{ -1 };
        RE::FormID actorFormID{ 0 };
        RE::ActorHandle handle;
        std::string actorName;
        bool loaded{ false };
        bool holder{ false };
        bool wearer{ false };
        bool unresolved{ false };
    };

    struct NprAssignmentInfo
    {
        std::uint8_t poolSlot{ 0 };
        RE::FormID actorFormID{ 0 };
        RE::ActorHandle handle;
        std::vector<std::string> contents;
        bool female{ false };
        bool unresolved{ false };
        bool restoreSuspended{ false };
    };

    struct NprSaveAssignment
    {
        std::uint8_t poolSlot{ 0 };
        RE::FormID actorFormID{ 0 };
        std::vector<std::string> contents;
        bool female{ false };
    };

    bool NpcEspLoaded();
    void InitializeNpcSupport();
    bool IsPublishToken(RE::FormID a_form);
    bool IsNpcPersistCarrier(RE::FormID a_form);
    bool IsCefToken(RE::FormID a_form);
    RE::TESObjectARMO* PubTokenArmo(int a_slot);
    RE::TESObjectARMO* NprTokenArmo(int a_slot);
    // Whether a published costume can become a box again right now, and why
    // not when it cannot. Unpublish refuses on anything but Ready rather than
    // looking for a substitute token: the substitute is a different biped slot,
    // and a costume's slot decides what it hides.
    enum class PubRestore
    {
        Ready,          // the reserved token is loaded and unclaimed
        NoSnapshot,     // nothing is published in that pool slot
        SourceUnknown,  // a pre-1.6.4 snapshot whose slot named no generation-0 token
        SourceMissing,  // the token's pool generation is not loaded (put it back)
        SourceTaken     // a box holds it - the reservation was lost (PLAN §5.1 rule 3)
    };
    [[nodiscard]] PubRestore PublishRestoreState(int a_pubSlot);
    [[nodiscard]] const char* PubRestoreReason(PubRestore a_state);

    // True while a published costume is holding this box token in reserve.
    // FreeTokens() asks on every candidate, so it stays a scan of at most eight
    // string compares and takes no lock of its own.
    [[nodiscard]] bool TokenReservedByPublish(const std::string& a_token);

    const PubSnapshot* PubBySlot(int a_slot);
    const PubSnapshot* PubByTokenForm(RE::FormID a_form);
    std::vector<PubSnapshot> PublishedSnapshot();

    // Ownership probes for ContentHolder: which published slot / NPC-persist
    // pool slot holds this content id, or -1. Separate from PublishedSnapshot()
    // on purpose - that one DEEP-COPIES every snapshot (settings + enchant maps),
    // and ContentHolder is called once per id inside the capture, preset and
    // side-map guards, which is exactly the per-row copy c439633 had to take
    // back out of the Recovery page.
    int PublishedSlotHolding(const std::string& a_content);
    int NpcPersistSlotHolding(const std::string& a_content);

    // Mark a published slot's synthesized stat ability stale, taking it off its
    // wearers first (its effect list may not be rewritten while it is applied).
    // The next ApplyBoxAbilities refills it from the CURRENT item-data toggles.
    //
    // Publish freezes a costume's APPEARANCE, but its stat passthrough is read
    // live on every rebuild - so an enchant/weight/armor toggle has to reach the
    // ability, and nothing marked it dirty outside unpublish and the settings
    // load (review follow-up 2026-09-10).
    //
    // It restamps the token too. Marking the ability stale only covers the
    // ENCHANT channel; armor and weight are written onto the token ARMO's own
    // fields, and nothing rewrote those - so turning armor off for a published
    // piece left the old rating on the token until some unrelated reload
    // happened to restamp it (review 2026-09-11 F03).
    void RefreshPublishedStats(int a_pubSlot);

    // Take every published slot's ability back off its wearers and the player,
    // and mark them stale. Called from InvalidateStatAbilities on game load, for
    // the same reason boxes and persist are: the effects are derived from THIS
    // save's hidden store, so another save's build must not be carried over.
    void InvalidatePublishAbilities();

    void EmitPublishJson(nlohmann::json& a_doc);
    // All or nothing: the published list is replaced only once the whole of it
    // has parsed. A throw leaves the previous list in place for the caller's
    // failure path to drop.
    void ParsePublishJson(const nlohmann::json& a_doc);
    // The settings file did not load: forget every published snapshot, as the
    // box store forgets its boxes, and put the pool tokens back to their
    // unpublished state. Keeps the co-save's per-save hidden flags.
    void DropPublishedSnapshots();
    void SaveGlobalSettings();

    // By boxId, not by list position. The caller is a UI row whose click and
    // whose confirmation are separated by a modal and a task hop, and a box
    // list that shifts in between would publish - freeze and remove - a
    // DIFFERENT box. boxId is what v1.6.4 introduced so nothing has to be
    // addressed by where it currently sits.
    bool PublishBox(const std::string& a_boxId);
    bool SetPubHidden(int a_slot, bool a_hidden);
    // Take a published costume's token and ability back off everyone wearing it.
    //
    // A wearer CEF cannot resolve right now is KEPT as a pending binding, not
    // forgotten: nothing scans for a forgotten one again (both DropPubAbility
    // and SyncNpcAbilities walk the binding list), and the unresolved list is
    // also what stops UnpublishToBox dissolving a costume somebody still has
    // (review 2026-09-11 F07).
    //
    // a_forgetUnreachable is for the uninstall flow only, where a live binding
    // that would ride the next co-save is worse than a forgotten one.
    // (Unresolved bindings never ride it since review 2026-09-23 C5 - they live
    // for the session, as the Unpublish guard.)
    bool RecallPublished(int a_slot, bool a_forgetUnreachable = false);
    // Let go of the bindings on a_slot that cannot be reached: unresolved ones,
    // ones whose actor is not in the game, and ones carried without a costume.
    // An actor that never loads again otherwise blocks Unpublish for good
    // (review 2026-09-23 W19). Returns how many went. Main thread.
    int ForgetUnreachablePub(int a_slot);
    // Remove the NPC-persist assignment on a pool slot, resolved or not - the
    // way to free a slot whose NPC's plugin is gone (W19). A reachable actor
    // goes through RemoveNpcPersist. Main thread.
    bool ForgetNprAssignment(int a_poolSlot);
    bool PubHidden(int a_slot);
    bool UnpublishToBox(int a_slot);
    void RefreshPubWearers(int a_slot);
    void StampAllPublishTokens();

    void OnNpcTokenEquip(RE::ActorHandle a_handle, RE::FormID a_base, bool a_equipped);
    bool SetNpcTokenWorn(RE::ActorHandle a_handle, int a_slot, bool a_worn);
    std::vector<PubBindingInfo> PubBindingsSnapshot();
    void OnPublishTokenMoved(RE::FormID a_base, RE::FormID a_from, RE::FormID a_to);
    void ReapplyNpcBindings();
    // What a published costume froze this content as worth at publish time, or
    // empty when no published costume holds it.
    //
    // Keyed by CONTENT rather than handed down from a holder, because the
    // fallback is a property of the content's owner and a content has exactly
    // one owner. That lets ContentEffectsFor answer for any id without the
    // caller knowing whether it came from a box or a costume - which is what
    // stops the pool needing a different entry point per holder.
    std::vector<EnchantEffectInfo> FrozenEffectsForContent(const std::string& a_contentId);

    // The contents an actor should be paid stats for through the publish
    // system: empty unless it is a wearer and publishing is grantable right now.
    std::vector<std::string> PublishStatsFor(RE::Actor* a_actor);

    void SyncNpcAbilities();
    bool HasNprWork();
    bool AssignNpcPersist(RE::Actor* a_actor, const std::vector<std::string>& a_contents);
    bool RemoveNpcPersist(RE::Actor* a_actor);
    // Manual FSMP-convergence driver for an npc-persist assignment: split-frame
    // carrier re-equip + clears a restoreSuspended park. False if the actor has
    // no assignment. Main thread only.
    bool RefreshNpcPersist(RE::Actor* a_actor);
    // In-place contents edit of an existing assignment: keeps the pool slot,
    // re-freezes settings, re-bakes the carrier and re-equips. False if the
    // actor has no assignment or the selection is empty/unresolvable.
    bool UpdateNpcPersist(RE::Actor* a_actor, const std::vector<std::string>& a_contents);
    void RestoreNpcPersistWear();
    void OnNpcActorLoaded(RE::ActorHandle a_handle);
    std::vector<NprAssignmentInfo> NprAssignmentsSnapshot();

    // What the co-save writes: the assignments whose actor resolved on THIS
    // load. The unresolved ones are NOT written (review 2026-09-23 C5): SKSE
    // re-reads a raw FormID through the plugin list of the save that wrote it,
    // and that list no longer has the missing plugin - so a value carried
    // across a save lands, on the next load, in whatever plugin moved into its
    // index: an unrelated NPC gets a carrier equipped and kept in its inventory.
    // There is no way to recover the plugin name from the raw value, and the
    // engine drops that NPC's inventory changes on such a save anyway.
    std::vector<NprSaveAssignment> NprAssignmentsForSave();
    // Everything that holds NPC-persist contents this session - resolved AND
    // unresolved - for the orphan sweep's held set and the holder labels. The
    // unresolved ones still hold their items until the save that drops them.
    std::vector<NprSaveAssignment> NprAssignmentsHeld();
    void RestoreNprAssignment(std::uint8_t a_slot, RE::FormID a_actor,
        std::vector<std::string> a_contents, bool a_female);
    // Kept in memory for this session only (slot reservation, held set) - see
    // NprAssignmentsForSave.
    void CarryUnresolvedNprAssignment(std::uint8_t a_slot, RE::FormID a_actor,
        std::vector<std::string> a_contents, bool a_female);
    std::size_t UnresolvedNprAssignmentCount();
    // Once per load, after the co-save restore: tell the player how many NPC
    // persist assignments belong to a plugin that is not loaded, because the
    // next save releases them (N3). Main thread.
    void ReportUnresolvedNpcRecords();
    void ClearNpcBindings();
    // "Prepare for uninstall" NPC arm: recall every published slot + remove
    // every npc-persist assignment (unresolvable ones dropped, loudly). No-op
    // without the addon esp. Main thread only.
    void UninstallNpcCleanup();

    // Every hidden flag the co-save restored, not only those of slots with a
    // costume this session: with the settings unreadable there are no
    // costumes, and writing only theirs erased every flag from the save
    // (review 2026-09-23 C8).
    std::vector<PubSaveState> PubStatesForSave();
    // The bindings to write: the live ones, plus the ones that resolved on this
    // load but whose costume is not loaded this session (C8 - values of THIS
    // load order, so safe to carry). Never the unresolved raw values (C5, see
    // NprAssignmentsForSave).
    std::vector<PubBindSave> PubBindingsForSave();
    void RestorePubState(std::uint8_t a_slot, bool a_hidden);
    // a_actor has been resolved through this save's plugin list. A value that
    // lands on something other than an actor was written by a save whose list
    // differs - the C5 corruption - and is dropped; the holder / wearer bits are
    // kept only where the actor really has / wears the token (R4b).
    void RestorePubBinding(std::uint8_t a_slot, RE::FormID a_actor, std::uint8_t a_flags);
    // Session-only, like CarryUnresolvedNprAssignment: still blocks
    // UnpublishToBox for that slot, never written.
    void CarryUnresolvedPubBinding(std::uint8_t a_slot, RE::FormID a_actor, std::uint8_t a_flags);
    std::size_t UnresolvedPubBindingCount();
    int MaxNpcInjected();
    std::vector<std::string> NpcDiagLines();
}


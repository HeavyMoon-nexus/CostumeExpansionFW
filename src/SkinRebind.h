#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace RE
{
    class BGSHeadPart;
}

namespace CostumeFW
{
    // One persisted active item: its content id and (for box items) the box
    // token's colon-form id (empty for persist items).
    struct ActiveItemInfo
    {
        std::string id;
        std::string tokenId;
    };

    // Inject a skinned NIF onto the player skeleton(s) (3rd + 1st person),
    // bypassing the equip/biped system. The attached node is named
    // "CostumeFW_<id>" for idempotency and detach. Returns true if attached on
    // at least one skeleton. MUST be called on the main thread.
    bool InjectSkinned(const std::string& a_nifPath, const std::string& a_id);

    // Resolve an ARMA (or ARMO -> its first ARMA) by local FormID + plugin name,
    // then inject its female model NIF AND apply its alternate texture set (the
    // ESP's color variant). This is the framework-correct path: seed id == arma
    // FormKey, and the ARMA carries both the NIF path and the texture variant.
    bool InjectArma(std::uint32_t a_localID, const std::string& a_plugin, const std::string& a_id);

    // Resolve + register an ARMA item by its colon-form id ("XXXXXX:Plugin.esp")
    // WITHOUT injecting (the co-save load path; Reconcile injects afterward).
    bool RegisterArmaById(const std::string& a_id);

    // Resolve + register + inject a persist item by its colon-form id
    // ("XXXXXX:Plugin.esp"). The Papyrus RegisterPersist() native path. Main
    // thread only (mutates the player scene graph).
    bool InjectArmaById(const std::string& a_id);

    // Define a box item: content (a colon-form ARMA id) shown only while the box
    // token (a colon-form ARMO id) is worn. Resolves + registers + reconciles.
    bool DefineBox(const std::string& a_contentId, const std::string& a_tokenId);

    // Box register without reconciling (co-save load path).
    bool RegisterBoxById(const std::string& a_contentId, const std::string& a_tokenId);

    // True if the FormID is a box token referenced by some active item (for the
    // equip-event filter).
    bool IsTrackedToken(std::uint32_t a_form);

    // Resolve a colon-form id "XXXXXX:Plugin.esp" to its full runtime FormID
    // (0 on failure). Shared by the box store (worn-token check / MCM equip).
    std::uint32_t ResolveFormId(const std::string& a_colonId);

    // Pre-capture guard (review item 2): TRUE if the content id resolves to a
    // usable mesh RIGHT NOW - the same (static per session) checks the queued
    // registration makes. The MCM refuses a capture BEFORE moving the item when
    // this fails, closing the "def added + item stored + nothing shows" window.
    // Pure data reads; safe on the Papyrus VM thread.
    bool CanResolveContent(const std::string& a_contentId);

    // ROOT D (border audit 2026-07-09): normalize a colon-form id to its canonical
    // "XXXXXX:Plugin.esp" spelling (upper-hex, 6 digits, no 0x / leading-zero
    // variance). True if the string changed. Every ingest border canonicalizes so
    // the string-keyed guards (ContentHolder, dedup, active-vs-catalog, per-content
    // side maps, injected node name) match ids that arrived non-canonical via a
    // hand-edited JSON, a shared preset, or another mod's direct native call.
    bool CanonicalizeColonId(std::string& a_id);

    // The body sex a_id's model resolves for (player sex overridden by the
    // per-content forced-gender mode). Exported so the carrier manifest picks
    // the SAME NIF the injection shows (review 2026-07-07 P2).
    RE::SEX EffectiveSexFor(const std::string& a_id);

    // The ARMA of an ARMO best matching the PLAYER's race: exact race match
    // first, then additionalRaces membership, then the first addon (data
    // order). Null when the ARMO has no addons. Replaces bare
    // armorAddons.front(), which picked race-/sex-specific addon lists wrong
    // (review item 6). Used by injection AND the carrier manifest.
    RE::TESObjectARMA* PickAddonForPlayer(RE::TESObjectARMO* a_armo);

    // Run a_fn on the main thread after (at least) a_ms milliseconds, via the
    // SKSE task queue (re-posts until the deadline; hop rate is NOT frame-locked).
    void RunAfterDelayMs(int a_ms, std::function<void()> a_fn);

    // Snapshot of active items (id + tokenId), and a registry clear (co-save).
    std::vector<ActiveItemInfo> ActiveSnapshot();
    void ClearRegistry();

    // Remove a previously injected node ("CostumeFW_<id>") from the player
    // skeleton(s) AND unregister it - Reconcile will NOT bring it back. Full
    // removal (cef detach / content removal). Main thread only.
    void DetachSkinned(const std::string& a_id);

    // Hide an injected item's scene nodes WITHOUT unregistering it - the next
    // Reconcile re-injects it fresh. For carrier swaps / forced re-binds where
    // the mesh must let go of soon-to-die FSMP bones. Main thread only.
    void HideInjectedNodes(const std::string& a_id);

    // Force one active item's injected model to re-resolve for its current
    // effective sex (after its forced-gender mode changed) and re-inject: detaches
    // the existing node (keeping the registration) so Reconcile re-attaches the
    // right-sex mesh. No-op if the id isn't registered. Main thread only.
    void RefreshGender(const std::string& a_id);

    // Reconcile every active item with the current player 3D: persist items are
    // (re)injected; box items are injected only while their token is worn, else
    // hidden. Called from the Load3D hook, kPostLoadGame, equip events, co-save
    // load. Idempotent. Main thread only.
    void Reconcile();

    // Detach + unregister every active item. Main thread only.
    void DetachAll();

    // Belt-and-suspenders: traverse BOTH player skeletons and detach EVERY node
    // whose name starts with "CostumeFW_", regardless of the registry. Catches
    // duplicate/orphaned holders that name-keyed detach (GetObjectByName returns
    // only the FIRST match) would leave rendering. Returns nodes removed. Main
    // thread only.
    int DetachAllInjected();

    // Log + console-print the active registry (for the `cef list` command).
    void ListActive();

    // Convenience for the PoC: reads the test NIF path from
    //   <Data>\SKSE\Plugins\CostumeExpansionFW_test.txt
    // and injects it with id "test". Logs every step.
    void InjectTestFromFile();
    void DetachTest();

    // --- approach-C persist head carriers (stage 3b) -------------------------
    // Reconcile the PLAYER base's headParts against the desired persist-pool
    // set. Adds go through TESNPC::ChangeHeadPart; removals are a direct
    // headParts array edit (ChangeHeadPart never REPLACES a same-type part -
    // in-game 2026-07-04, C §9-16 - so there is no engine removal to call).
    // a_pool is the full pool (carrier + all proxies) so stale members are
    // detected even when absent from a_desired. Registration is save-persisted
    // (player appearance); flags the NPC face-changed on any edit. Does NOT
    // rebuild the head - pair with RebuildPlayerHead(). Returns true if the
    // registration changed. Main thread only.
    bool ReconcilePersistHeadParts(const std::vector<RE::BGSHeadPart*>& a_desired,
        const std::vector<RE::BGSHeadPart*>& a_pool);

    // True if the player base's headParts currently contains a_part (for the
    // `cef persist` status print).
    bool PlayerHasHeadPart(RE::BGSHeadPart* a_part);

    // v1.2.1 plugin-merge migration: deregister every "CFW_*" head part that is
    // NOT a member of the merged plugin's pool - i.e. old-pool / PoC parts of the
    // pre-merge plugins, resolvable while those plugins are still enabled for the
    // one transition load (disabled old plugins never reach the runtime array, so
    // this is a no-op then). Returns true if something was removed (the caller
    // pairs it with a head rebuild). Main thread only.
    bool SweepLegacyCfwHeadParts(const std::vector<RE::BGSHeadPart*>& a_currentPool);

    // Debounced request for a persist head rebuild: coalesces a burst of
    // ApplyPersistCarrier-driven rebuilds into ONE DoReset3D ~500ms after the
    // last request, so FSMP rebuilds the wig physics once (not per settings-write
    // / sync / load pass) - each FSMP rebuild commits a ~2.5GB SSE Engine Fixes
    // arena that is retained. Use this from the automatic carrier path; the
    // manual levers (cef persist regen/remove) call RebuildPlayerHead directly.
    // Main thread only.
    void RequestPersistHeadRebuild(const char* a_reason);

    // One-line churn diagnostics (head-rebuild coalescing, reconcile / watchdog /
    // dead-bind / rebind-retry / body-morph-apply counts) for `cef persist`.
    std::string PersistDiagString();

    // Force a facegen head rebuild (DoReset3D) so the engine loads the current
    // head-part models and FSMP's facegen path re-fires, then re-inject after a
    // beat: DoReset3D does NOT fire the Load3D hook (C §9-11(ii)), so the
    // Reconcile must be explicit; the bind watchdog converges any merge
    // generation churn after that. No-op while the player has no 3D (load
    // time: the engine builds the head with the current registration anyway).
    // Main thread only.
    void RebuildPlayerHead();

    // FSMP approach-C active PoC (stage 1, `cef hair <FormID:Plugin.esp>`): drive
    // a head-part change from CEF code (TESNPC::ChangeHeadPart) + force a facegen
    // rebuild (DoReset3D) to test whether FSMP's facegen path (2) enumerates a
    // code-changed head part. Pass a known SMP-hair HDPT, then `cef headdiag`.
    // NOTE: ChangeHeadPart is save-persistent (like RaceMenu) - reversible by
    // re-applying the original hair or reloading. Main thread only.
    bool ChangeHeadPartPoC(const std::string& a_id);

    // FSMP approach-C passive PoC (`cef headdiag`): walk BOTH player skeletons
    // and enumerate every FSMP-renamed physics bone
    // ("hdtSSEPhysics_AutoRename_(Armor|Head)_<8hex> <bone>"), grouped by the
    // <prefix>_<id> the bone was merged under. Confirms whether the facegen head
    // path (SkinAllGeometry -> SkinSingleHeadGeometryEvent) fired and produced
    // "_Head_" bones (e.g. from an equipped SMP hair) that the injection rebind
    // can then target. Console-prints a summary + logs the full list. Main thread.
    void HeadDiag();
}

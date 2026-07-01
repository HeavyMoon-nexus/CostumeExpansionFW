#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

    // Snapshot of active items (id + tokenId), and a registry clear (co-save).
    std::vector<ActiveItemInfo> ActiveSnapshot();
    void ClearRegistry();

    // Remove a previously injected node ("CostumeFW_<id>") from the player
    // skeleton(s). Main thread only.
    void DetachSkinned(const std::string& a_id);

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

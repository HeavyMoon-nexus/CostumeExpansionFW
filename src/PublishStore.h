#pragma once

#include "SkinRebind.h"

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
    const PubSnapshot* PubBySlot(int a_slot);
    const PubSnapshot* PubByTokenForm(RE::FormID a_form);
    std::vector<PubSnapshot> PublishedSnapshot();

    void EmitPublishJson(nlohmann::json& a_doc);
    void ParsePublishJson(const nlohmann::json& a_doc);
    void SaveGlobalSettings();

    bool PublishBox(int a_boxIndex);
    bool SetPubHidden(int a_slot, bool a_hidden);
    bool RecallPublished(int a_slot);
    bool PubHidden(int a_slot);
    bool UnpublishToBox(int a_slot);
    void RefreshPubWearers(int a_slot);
    void StampAllPublishTokens();

    void OnNpcTokenEquip(RE::ActorHandle a_handle, RE::FormID a_base, bool a_equipped);
    bool SetNpcTokenWorn(RE::ActorHandle a_handle, int a_slot, bool a_worn);
    std::vector<PubBindingInfo> PubBindingsSnapshot();
    void OnPublishTokenMoved(RE::FormID a_base, RE::FormID a_from, RE::FormID a_to);
    void ReapplyNpcBindings();
    void SyncNpcAbilities();
    bool HasNprWork();
    bool AssignNpcPersist(RE::Actor* a_actor, const std::vector<std::string>& a_contents);
    bool RemoveNpcPersist(RE::Actor* a_actor);
    void RestoreNpcPersistWear();
    void OnNpcActorLoaded(RE::ActorHandle a_handle);
    std::vector<NprAssignmentInfo> NprAssignmentsSnapshot();

    std::vector<NprSaveAssignment> NprAssignmentsForSave();
    void RestoreNprAssignment(std::uint8_t a_slot, RE::FormID a_actor,
        std::vector<std::string> a_contents, bool a_female);
    void CarryUnresolvedNprAssignment(std::uint8_t a_slot, RE::FormID a_actor,
        std::vector<std::string> a_contents, bool a_female);
    std::size_t UnresolvedNprAssignmentCount();
    void ClearNpcBindings();

    std::vector<PubSaveState> PubStatesForSave();
    std::vector<PubBindSave> PubBindingsForSave();
    void RestorePubState(std::uint8_t a_slot, bool a_hidden);
    void RestorePubBinding(std::uint8_t a_slot, RE::FormID a_actor, std::uint8_t a_flags);
    void CarryUnresolvedPubBinding(std::uint8_t a_slot, RE::FormID a_actor, std::uint8_t a_flags);
    std::size_t UnresolvedPubBindingCount();
    int MaxNpcInjected();
    std::vector<std::string> NpcDiagLines();
}


#include "PublishStore.h"

#include "BoxStore.h"
#include "Config.h"
#include "SkinRebind.h"

#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/I/IFormFactory.h"
#include "RE/S/SpellItem.h"
#include <nlohmann/json.hpp>

namespace CostumeFW
{
    namespace
    {
        constexpr const char* kNpcPlugin = "CostumeFW_NPC.esp";
        constexpr std::uint32_t kPubTokenFirst = 0x800;
        constexpr std::uint32_t kNprTokenFirst = 0x810;
        constexpr int kPoolSize = 8;

        struct PubBinding
        {
            int pubSlot{ -1 };
            RE::FormID actorFormID{ 0 };
            RE::ActorHandle handle;
            bool holder{ false };
            bool wearer{ false };
        };

        std::vector<std::shared_ptr<PubSnapshot>> g_published;
        std::vector<PubBinding> g_bindings;
        std::vector<PubBindSave> g_unresolved;
        std::unordered_map<int, bool> g_hidden;
        std::vector<NprAssignmentInfo> g_nprAssignments;
        std::vector<NprSaveAssignment> g_unresolvedNpr;
        std::unordered_set<RE::FormID> g_pubForms;
        std::unordered_set<RE::FormID> g_nprForms;
        int g_maxNpcInjected = 8;
        std::unordered_map<int, std::vector<RE::BGSKeyword*>> g_pubKeywords;
        std::unordered_map<int, RE::SpellItem*> g_pubEnchantSpells;

        RE::Actor* ResolveActor(PubBinding& a_binding)
        {
            if (auto ref = a_binding.handle.get()) {
                if (auto* actor = ref.get()->As<RE::Actor>()) return actor;
            }
            auto* form = RE::TESForm::LookupByID(a_binding.actorFormID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (actor) a_binding.handle = actor->GetHandle();
            return actor;
        }

        std::string PubTokenId(int a_slot)
        {
            char id[64]{};
            std::snprintf(id, sizeof(id), "%06X:%s", kPubTokenFirst + a_slot, kNpcPlugin);
            return id;
        }
        std::string NprTokenId(int a_slot)
        {
            char id[64]{};
            std::snprintf(id, sizeof(id), "%06X:%s", kNprTokenFirst + a_slot, kNpcPlugin);
            return id;
        }

        template <class T>
        T* ResolveColonForm(const std::string& a_id)
        {
            const auto colon = a_id.find(':');
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data || colon == std::string::npos) return nullptr;
            const auto local = static_cast<std::uint32_t>(
                std::strtoul(a_id.substr(0, colon).c_str(), nullptr, 16));
            return data->LookupForm<T>(local, a_id.substr(colon + 1));
        }

        bool PublishKeywordAllowed(std::string_view a_editorId)
        {
            for (const auto prefix : { "OCF_", "ArmorMaterial", "WeapType", "WAF_" })
                if (a_editorId.starts_with(prefix)) return false;
            static const std::unordered_set<std::string_view> blocked{
                "ArmorHeavy", "ArmorLight", "ArmorClothing", "ArmorJewelry",
                "ArmorCuirass", "ArmorBoots", "ArmorGauntlets", "ArmorHelmet",
                "ArmorShield", "ArmorBracer", "ArmorCirclet",
                "ClothingBody", "ClothingHead", "ClothingFeet", "ClothingHands",
                "ClothingRing", "ClothingNecklace", "ClothingCirclet",
                "VendorItemJewelry", "VendorItemClothing", "VendorItemArmor"
            };
            return !blocked.contains(a_editorId);
        }

        void StampSnapshotStats(const PubSnapshot& a_snap)
        {
            auto* token = PubTokenArmo(a_snap.pubSlot);
            if (!token) return;
            auto& prior = g_pubKeywords[a_snap.pubSlot];
            for (auto* keyword : prior) token->RemoveKeyword(keyword);
            prior.clear();
            float armor = 0.0f;
            float weight = 0.0f;
            for (const auto& id : a_snap.contents) {
                auto* item = ResolveColonForm<RE::TESObjectARMO>(id);
                if (!item) continue;
                armor += item->GetArmorRating();
                weight += item->weight;
                for (auto* keyword : item->GetKeywords()) {
                    if (!keyword) continue;
                    const char* editorId = keyword->formEditorID.c_str();
                    if (editorId && *editorId && PublishKeywordAllowed(editorId) &&
                        token->AddKeyword(keyword)) {
                        prior.push_back(keyword);
                    }
                }
            }
            token->armorRating = static_cast<std::uint32_t>(armor * 100.0f);
            token->weight = weight;
            using AT = RE::BIPED_MODEL::ArmorType;
            token->bipedModelData.armorType = a_snap.armorType == 1 ? AT::kLightArmor :
                (a_snap.armorType == 2 ? AT::kHeavyArmor : AT::kClothing);
        }

        void ResetPublishedTokenState(int a_slot)
        {
            auto* token = PubTokenArmo(a_slot);
            if (token) {
                if (auto it = g_pubKeywords.find(a_slot); it != g_pubKeywords.end()) {
                    for (auto* keyword : it->second) token->RemoveKeyword(keyword);
                    g_pubKeywords.erase(it);
                }
                token->fullName = "Costume (unpublished)";
                token->armorRating = 0;
                token->weight = 0.0f;
                token->bipedModelData.armorType = RE::BIPED_MODEL::ArmorType::kClothing;
                const auto mask = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(
                    1u << (44 - 30));
                token->bipedModelData.bipedObjectSlots = mask;
                for (auto* addon : token->armorAddons)
                    if (addon) addon->bipedModelData.bipedObjectSlots = mask;
            }
            g_pubEnchantSpells.erase(a_slot);
            g_hidden.erase(a_slot);
        }

        RE::SpellItem* PublishedEnchantAbility(const PubSnapshot& a_snap)
        {
            if (const auto it = g_pubEnchantSpells.find(a_snap.pubSlot);
                it != g_pubEnchantSpells.end()) return it->second;
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
            auto* spell = factory ? factory->Create() : nullptr;
            if (!spell) {
                g_pubEnchantSpells[a_snap.pubSlot] = nullptr;
                return nullptr;
            }
            for (const auto& [id, effects] : a_snap.enchants) {
                (void)id;
                for (const auto& frozen : effects) {
                    auto* mgef = ResolveColonForm<RE::EffectSetting>(frozen.mgef);
                    if (!mgef) continue;
                    auto* effect = new RE::Effect();
                    effect->baseEffect = mgef;
                    effect->effectItem.magnitude = frozen.magnitude;
                    effect->effectItem.area = 0;
                    effect->effectItem.duration = 0;
                    spell->effects.push_back(effect);
                }
            }
            if (spell->effects.empty()) {
                delete spell;
                spell = nullptr;
            } else {
                spell->data.spellType = RE::MagicSystem::SpellType::kAbility;
                spell->data.castingType = RE::MagicSystem::CastingType::kConstantEffect;
                spell->data.delivery = RE::MagicSystem::Delivery::kSelf;
                spell->data.costOverride = 0;
                spell->fullName = ("Costume Stats: " + a_snap.label).c_str();
            }
            g_pubEnchantSpells[a_snap.pubSlot] = spell;
            return spell;
        }

        void ApplyManualAbility(RE::Actor* a_actor, const PubSnapshot& a_snap, bool a_equip)
        {
            if (!a_actor) return;
            if (!a_snap.manualAbility.empty()) {
                if (auto* spell = ResolveColonForm<RE::SpellItem>(a_snap.manualAbility)) {
                    if (a_equip) a_actor->AddSpell(spell);
                    else a_actor->RemoveSpell(spell);
                }
            }
            if (auto* spell = PublishedEnchantAbility(a_snap)) {
                if (a_equip) a_actor->AddSpell(spell);
                else a_actor->RemoveSpell(spell);
            }
        }

        std::shared_ptr<PubSnapshot> SharedBySlot(int a_slot)
        {
            for (auto& snap : g_published) if (snap->pubSlot == a_slot) return snap;
            return {};
        }

        PubBinding& EnsureBinding(int a_slot, RE::Actor* a_actor)
        {
            for (auto& binding : g_bindings) {
                if (binding.pubSlot == a_slot && binding.actorFormID == a_actor->GetFormID()) {
                    binding.handle = a_actor->GetHandle();
                    return binding;
                }
            }
            g_bindings.push_back({ a_slot, a_actor->GetFormID(), a_actor->GetHandle() });
            return g_bindings.back();
        }

        bool RegisterSnapshot(RE::Actor* a_actor, const PubSnapshot& a_snap)
        {
            auto token = PubTokenArmo(a_snap.pubSlot);
            if (!a_actor || !token) return false;
            bool any = false;
            for (const auto& id : a_snap.contents) {
                auto it = a_snap.settings.find(id);
                std::shared_ptr<const ContentSettings> settings =
                    it == a_snap.settings.end() ? std::make_shared<ContentSettings>() : it->second;
                any |= RegisterActorContent(a_actor, id, PubTokenId(a_snap.pubSlot),
                    token->GetFormID(), std::move(settings));
            }
            return any;
        }

        RE::Actor* ResolveNprActor(NprAssignmentInfo& a_assignment)
        {
            if (auto ref = a_assignment.handle.get()) {
                if (auto* actor = ref.get()->As<RE::Actor>()) return actor;
            }
            auto* form = RE::TESForm::LookupByID(a_assignment.actorFormID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (actor) a_assignment.handle = actor->GetHandle();
            return actor;
        }

        bool RegisterNpr(RE::Actor* a_actor, const NprAssignmentInfo& a_assignment)
        {
            auto* token = NprTokenArmo(a_assignment.poolSlot);
            if (!a_actor || !token) return false;
            if (!HasActorBindings(a_actor) &&
                InjectedNpcCount() >= static_cast<std::size_t>(g_maxNpcInjected))
                return false;
            bool any = false;
            for (const auto& id : a_assignment.contents) {
                any |= RegisterActorContent(a_actor, id, NprTokenId(a_assignment.poolSlot),
                    token->GetFormID(), {});
            }
            return any;
        }

        NprAssignmentInfo* FindNpr(RE::FormID a_actor, int a_slot)
        {
            for (auto& item : g_nprAssignments) {
                if (item.actorFormID == a_actor && item.poolSlot == a_slot) return &item;
            }
            return nullptr;
        }

        bool ActorHasItem(RE::Actor* a_actor, RE::TESBoundObject* a_item)
        {
            if (!a_actor || !a_item) return false;
            const auto inventory = a_actor->GetInventory();
            const auto it = inventory.find(a_item);
            return it != inventory.end() && it->second.first > 0;
        }

        void ScheduleNprRestore(RE::FormID a_actor, int a_slot, int a_attempt)
        {
            if (a_attempt >= 3) {
                if (auto* item = FindNpr(a_actor, a_slot)) item->restoreSuspended = true;
                return;
            }
            std::thread([a_actor, a_slot, a_attempt] {
                std::this_thread::sleep_for(std::chrono::seconds(30));
                SKSE::GetTaskInterface()->AddTask([a_actor, a_slot, a_attempt] {
                    auto* item = FindNpr(a_actor, a_slot);
                    if (!item || item->restoreSuspended || !CefEnabled()) return;
                    auto* actor = ResolveNprActor(*item);
                    auto* token = NprTokenArmo(a_slot);
                    auto* equip = RE::ActorEquipManager::GetSingleton();
                    if (!actor || !token || !equip) return;
                    if (actor->GetWornArmor(token->GetFormID())) return;
                    if (!ActorHasItem(actor, token))
                        actor->AddObjectToContainer(token, nullptr, 1, nullptr);
                    equip->EquipObject(actor, token, nullptr, 1, nullptr, true, false, false);
                    if (!actor->GetWornArmor(token->GetFormID()))
                        ScheduleNprRestore(a_actor, a_slot, a_attempt + 1);
                });
            }).detach();
        }

        void ReconcileSlot(int a_slot)
        {
            for (auto& binding : g_bindings) {
                if (binding.pubSlot != a_slot) continue;
                if (auto* actor = ResolveActor(binding)) ReconcileActorByHandle(actor->GetHandle());
            }
        }
    }

    bool NpcEspLoaded()
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        return data && data->LookupForm<RE::TESObjectARMO>(kPubTokenFirst, kNpcPlugin);
    }

    void InitializeNpcSupport()
    {
        g_pubForms.clear();
        g_nprForms.clear();
        if (!NpcEspLoaded()) return;
        auto* data = RE::TESDataHandler::GetSingleton();
        for (int i = 0; i < kPoolSize; ++i) {
            if (auto* form = data->LookupForm<RE::TESObjectARMO>(kPubTokenFirst + i, kNpcPlugin))
                g_pubForms.insert(form->GetFormID());
            if (auto* form = data->LookupForm<RE::TESObjectARMO>(kNprTokenFirst + i, kNpcPlugin))
                g_nprForms.insert(form->GetFormID());
        }
        StampAllPublishTokens();
    }

    bool IsPublishToken(RE::FormID a_form) { return g_pubForms.contains(a_form); }
    bool IsNpcPersistCarrier(RE::FormID a_form) { return g_nprForms.contains(a_form); }
    bool IsCefToken(RE::FormID a_form)
    {
        return IsBoxToken(a_form) || IsPublishToken(a_form) || IsNpcPersistCarrier(a_form);
    }
    RE::TESObjectARMO* NprTokenArmo(int a_slot)
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        return data && a_slot >= 0 && a_slot < kPoolSize ?
            data->LookupForm<RE::TESObjectARMO>(kNprTokenFirst + a_slot, kNpcPlugin) : nullptr;
    }

    RE::TESObjectARMO* PubTokenArmo(int a_slot)
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        return data && a_slot >= 0 && a_slot < kPoolSize ?
            data->LookupForm<RE::TESObjectARMO>(kPubTokenFirst + a_slot, kNpcPlugin) : nullptr;
    }

    const PubSnapshot* PubBySlot(int a_slot)
    {
        auto snap = SharedBySlot(a_slot);
        return snap.get();
    }

    const PubSnapshot* PubByTokenForm(RE::FormID a_form)
    {
        for (const auto& snap : g_published) {
            auto* token = PubTokenArmo(snap->pubSlot);
            if (token && token->GetFormID() == a_form) return snap.get();
        }
        return nullptr;
    }

    std::vector<PubSnapshot> PublishedSnapshot()
    {
        std::vector<PubSnapshot> out;
        for (const auto& snap : g_published) out.push_back(*snap);
        return out;
    }

    void EmitPublishJson(nlohmann::json& a_doc)
    {
        auto published = nlohmann::json::array();
        for (const auto& snap : g_published) {
            nlohmann::json item{
                { "pubSlot", snap->pubSlot }, { "label", snap->label },
                { "sourceSlot", snap->sourceSlot }, { "armorType", snap->armorType },
                { "manualAbility", snap->manualAbility }, { "rev", snap->rev },
                { "contents", snap->contents }
            };
            auto& settings = item["settings"];
            for (const auto& [id, cfg] : snap->settings) {
                if (!cfg) continue;
                settings["hideRules"][id] = cfg->hideSlots;
                settings["genderModes"][id] = cfg->genderMode;
                if (cfg->bodyMorph) settings["bodyMorph"].push_back(id);
                settings["hideShapes"][id] = cfg->hideShapes;
                if (cfg->showRealBody) settings["showRealBody"].push_back(id);
            }
            for (const auto& [id, effects] : snap->enchants) {
                auto frozen = nlohmann::json::array();
                for (const auto& effect : effects)
                    frozen.push_back({ { "mgef", effect.mgef }, { "mag", effect.magnitude } });
                settings["enchants"][id] = std::move(frozen);
            }
            published.push_back(std::move(item));
        }
        a_doc["published"] = std::move(published);
        a_doc["npcConfig"]["maxNpcInjected"] = g_maxNpcInjected;
    }

    void ParsePublishJson(const nlohmann::json& a_doc)
    {
        g_published.clear();
        g_pubEnchantSpells.clear();
        g_maxNpcInjected = std::clamp(a_doc.value("npcConfig", nlohmann::json::object())
            .value("maxNpcInjected", 8), 1, 64);
        std::unordered_set<int> slots;
        for (const auto& item : a_doc.value("published", nlohmann::json::array())) {
            auto snap = std::make_shared<PubSnapshot>();
            snap->pubSlot = item.value("pubSlot", -1);
            if (snap->pubSlot < 0 || snap->pubSlot >= kPoolSize || !slots.insert(snap->pubSlot).second)
                continue;
            snap->label = item.value("label", std::string{});
            snap->sourceSlot = item.value("sourceSlot", 0);
            snap->armorType = item.value("armorType", 0);
            snap->manualAbility = item.value("manualAbility", std::string{});
            snap->rev = item.value("rev", 0);
            snap->contents = item.value("contents", std::vector<std::string>{});
            const auto settings = item.value("settings", nlohmann::json::object());
            for (auto& id : snap->contents) {
                CanonicalizeColonId(id);
                auto cfg = std::make_shared<ContentSettings>();
                cfg->hideSlots = settings.value("hideRules", nlohmann::json::object())
                    .value(id, std::vector<int>{});
                cfg->genderMode = settings.value("genderModes", nlohmann::json::object())
                    .value(id, 0);
                const auto morph = settings.value("bodyMorph", std::vector<std::string>{});
                cfg->bodyMorph = std::find(morph.begin(), morph.end(), id) != morph.end();
                const auto shapes = settings.value("hideShapes", nlohmann::json::object())
                    .value(id, std::vector<std::string>{});
                cfg->hideShapes.insert(shapes.begin(), shapes.end());
                const auto real = settings.value("showRealBody", std::vector<std::string>{});
                cfg->showRealBody = std::find(real.begin(), real.end(), id) != real.end();
                snap->settings.emplace(id, std::move(cfg));
                const auto enchantMap = settings.value("enchants", nlohmann::json::object());
                if (const auto eit = enchantMap.find(id); eit != enchantMap.end() && eit->is_array()) {
                    for (const auto& effect : *eit) {
                        PubEnchantEffect frozen{
                            effect.value("mgef", std::string{}), effect.value("mag", 0.0f)
                        };
                        CanonicalizeColonId(frozen.mgef);
                        if (!frozen.mgef.empty()) snap->enchants[id].push_back(std::move(frozen));
                    }
                }
            }
            g_published.push_back(std::move(snap));
        }
        InitializeNpcSupport();
    }

    bool PublishBox(int a_boxIndex)
    {
        if (!NpcEspLoaded()) return false;
        const auto box = BoxAt(a_boxIndex);
        if (box.token.empty() || box.contents.empty()) return false;
        int slot = -1;
        for (int i = 0; i < kPoolSize; ++i) if (!PubBySlot(i)) { slot = i; break; }
        if (slot < 0) return false;
        auto snap = std::make_shared<PubSnapshot>();
        snap->pubSlot = slot;
        snap->label = box.label;
        snap->sourceSlot = TokenSlot(box.token);
        snap->armorType = box.armorType;
        snap->manualAbility = box.ability;
        snap->rev = 1;
        snap->contents = box.contents;
        for (const auto& id : box.contents) {
            auto cfg = std::make_shared<ContentSettings>();
            cfg->hideSlots = HideSlotsFor(id);
            cfg->genderMode = GenderModeFor(id);
            cfg->bodyMorph = BodyMorphOn(id);
            const auto shapes = HideShapesFor(id);
            cfg->hideShapes.insert(shapes.begin(), shapes.end());
            cfg->showRealBody = ShowRealBodyOn(id);
            snap->settings.emplace(id, std::move(cfg));
            for (const auto& effect : ContentEnchantSnapshot(id))
                snap->enchants[id].push_back({ effect.mgef, effect.magnitude });
        }
        ResetPublishedTokenState(slot);
        g_published.push_back(snap);
        WearBoxToken(box.token, false);
        for (const auto& id : box.contents) DetachSkinned(id);
        RemoveBox(box.token);
        GiveOrRemoveToken(box.token, false);
        StampAllPublishTokens();
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* token = PubTokenArmo(slot))
                player->AddObjectToContainer(token, nullptr, 1, nullptr);
        }
        SaveGlobalSettings();
        Reconcile();
        return true;
    }

    bool SetPubHidden(int a_slot, bool a_hidden)
    {
        if (!PubBySlot(a_slot)) return false;
        g_hidden[a_slot] = a_hidden;
        for (auto& binding : g_bindings) {
            if (binding.pubSlot != a_slot || !binding.wearer) continue;
            if (auto* actor = ResolveActor(binding)) {
                if (a_hidden) RemoveActorToken(actor, PubTokenArmo(a_slot)->GetFormID());
                else if (HasActorBindings(actor) ||
                    InjectedNpcCount() < static_cast<std::size_t>(g_maxNpcInjected)) {
                    RegisterSnapshot(actor, *PubBySlot(a_slot));
                }
                ReconcileActorByHandle(actor->GetHandle());
            }
        }
        return true;
    }

    bool PubHidden(int a_slot)
    {
        return g_hidden.contains(a_slot) && g_hidden[a_slot];
    }

    void OnNpcTokenEquip(RE::ActorHandle a_handle, RE::FormID a_base, bool a_equipped)
    {
        auto ref = a_handle.get();
        auto* actor = ref ? ref.get()->As<RE::Actor>() : nullptr;
        if (actor && IsNpcPersistCarrier(a_base)) {
            int slot = -1;
            for (int i = 0; i < kPoolSize; ++i) {
                auto* token = NprTokenArmo(i);
                if (token && token->GetFormID() == a_base) {
                    slot = i;
                    break;
                }
            }
            if (slot >= 0) {
                if (auto* item = FindNpr(actor->GetFormID(), slot)) {
                    if (a_equipped) {
                        item->restoreSuspended = false;
                        RegisterNpr(actor, *item);
                    } else {
                        RemoveActorToken(actor, a_base);
                        ScheduleNprRestore(actor->GetFormID(), slot, 0);
                    }
                    ReconcileActorByHandle(a_handle);
                }
            }
            return;
        }
        const auto* snap = PubByTokenForm(a_base);
        if (!actor || !snap) return;
        auto& binding = EnsureBinding(snap->pubSlot, actor);
        binding.holder = true;
        binding.wearer = a_equipped;
        const bool allowed = g_hidden[snap->pubSlot] || HasActorBindings(actor) ||
            InjectedNpcCount() < static_cast<std::size_t>(g_maxNpcInjected);
        ApplyManualAbility(actor, *snap, a_equipped && allowed);
        if (a_equipped && allowed && !g_hidden[snap->pubSlot])
            RegisterSnapshot(actor, *snap);
        if (!a_equipped) RemoveActorToken(actor, a_base);
        ReconcileActorByHandle(a_handle);
    }

    bool SetNpcTokenWorn(RE::ActorHandle a_handle, int a_slot, bool a_worn)
    {
        auto ref = a_handle.get();
        auto* actor = ref ? ref.get()->As<RE::Actor>() : nullptr;
        auto* token = PubTokenArmo(a_slot);
        auto* equip = RE::ActorEquipManager::GetSingleton();
        if (!actor || actor == RE::PlayerCharacter::GetSingleton() || !token || !equip ||
            !PubBySlot(a_slot)) {
            return false;
        }
        auto* binding = static_cast<PubBinding*>(nullptr);
        for (auto& item : g_bindings) {
            if (item.pubSlot == a_slot && item.actorFormID == actor->GetFormID()) {
                binding = &item;
                break;
            }
        }
        if (!binding || !binding->holder) return false;
        if (a_worn && !g_hidden[a_slot] && !binding->wearer && !HasActorBindings(actor) &&
            InjectedNpcCount() >= static_cast<std::size_t>(g_maxNpcInjected))
            return false;
        binding->handle = a_handle;
        if (a_worn) {
            binding->wearer = true;
            ApplyManualAbility(actor, *PubBySlot(a_slot), true);
            if (!g_hidden[a_slot]) RegisterSnapshot(actor, *PubBySlot(a_slot));
            equip->EquipObject(actor, token, nullptr, 1, nullptr, true, false, false);
        } else {
            binding->wearer = false;
            RemoveActorToken(actor, token->GetFormID());
            ApplyManualAbility(actor, *PubBySlot(a_slot), false);
            equip->UnequipObject(actor, token, nullptr, 1, nullptr, true, false, false);
        }
        ReconcileActorByHandle(a_handle);
        return true;
    }

    std::vector<PubBindingInfo> PubBindingsSnapshot()
    {
        std::vector<PubBindingInfo> out;
        out.reserve(g_bindings.size() + g_unresolved.size());
        for (const auto& binding : g_bindings) {
            PubBindingInfo item{ binding.pubSlot, binding.actorFormID, binding.handle, {},
                false, binding.holder, binding.wearer, false };
            if (auto ref = binding.handle.get()) {
                if (auto* actor = ref.get()->As<RE::Actor>()) {
                    item.loaded = true;
                    const char* name = actor->GetName();
                    if (name) item.actorName = name;
                }
            }
            out.push_back(std::move(item));
        }
        for (const auto& saved : g_unresolved) {
            out.push_back({ saved.pubSlot, saved.actorFormID, {}, {}, false,
                (saved.flags & 1) != 0, (saved.flags & 2) != 0, true });
        }
        return out;
    }

    void OnPublishTokenMoved(RE::FormID a_base, RE::FormID a_from, RE::FormID a_to)
    {
        const auto* snap = PubByTokenForm(a_base);
        if (!snap) return;
        if (auto* form = RE::TESForm::LookupByID(a_to)) {
            if (auto* actor = form->As<RE::Actor>()) EnsureBinding(snap->pubSlot, actor).holder = true;
        }
        for (auto& binding : g_bindings) {
            if (binding.pubSlot == snap->pubSlot && binding.actorFormID == a_from)
                binding.holder = false;
        }
        std::erase_if(g_bindings, [](const PubBinding& b) { return !b.holder && !b.wearer; });
    }

    void RefreshPubWearers(int a_slot)
    {
        auto* token = PubTokenArmo(a_slot);
        auto* equip = RE::ActorEquipManager::GetSingleton();
        if (!token || !equip) return;
        for (auto& binding : g_bindings) {
            if (binding.pubSlot != a_slot || !binding.wearer) continue;
            if (auto* actor = ResolveActor(binding)) {
                equip->UnequipObject(actor, token, nullptr, 1, nullptr, true, false, false);
                equip->EquipObject(actor, token, nullptr, 1, nullptr, true, false, false);
            }
        }
    }

    bool RecallPublished(int a_slot)
    {
        auto* token = PubTokenArmo(a_slot);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!token || !player) return false;
        for (auto& binding : g_bindings) {
            if (binding.pubSlot != a_slot) continue;
            if (auto* actor = ResolveActor(binding)) {
                RemoveActorToken(actor, token->GetFormID());
                if (const auto* snap = PubBySlot(a_slot)) ApplyManualAbility(actor, *snap, false);
                actor->RemoveItem(token, 99, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
            }
        }
        std::erase_if(g_bindings, [a_slot](const PubBinding& b) { return b.pubSlot == a_slot; });
        std::erase_if(g_unresolved, [a_slot](const PubBindSave& b) { return b.pubSlot == a_slot; });
        return true;
    }

    bool UnpublishToBox(int a_slot)
    {
        auto snap = SharedBySlot(a_slot);
        if (!snap) return false;
        for (const auto& binding : g_unresolved)
            if (binding.pubSlot == a_slot) return false;
        // The PLAYER's own holder binding never blocks unpublish: Recall lands
        // every copy in the player's inventory, and the container event that
        // move fires re-creates a player holder binding - counting it here made
        // unpublish permanently unreachable after any recall/preview. Step 4
        // below removes the player's copies itself; only NON-player holders are
        // outside this function's reach and must be recalled first.
        auto* playerRef = RE::PlayerCharacter::GetSingleton();
        const RE::FormID playerID = playerRef ? playerRef->GetFormID() : 0x14;
        for (const auto& binding : g_bindings)
            if (binding.pubSlot == a_slot && binding.holder && binding.actorFormID != playerID)
                return false;
        auto token = NextFreeToken();
        if (token.empty()) return false;
        for (const auto& candidate : FreeTokens()) {
            if (TokenSlot(candidate) == snap->sourceSlot) {
                token = candidate;
                break;
            }
        }
        std::unordered_set<std::string> liveSettings;
        for (const auto& id : snap->contents)
            if (!ContentHolder(id).empty()) liveSettings.insert(id);
        AddBox(snap->label, token, {});
        for (const auto& id : snap->contents) {
            AddBox(snap->label, token, id);
            if (auto it = snap->settings.find(id);
                !liveSettings.contains(id) && it != snap->settings.end() && it->second) {
                SetHideSlots(id, it->second->hideSlots);
                SetGenderMode(id, it->second->genderMode);
                SetBodyMorphOn(id, it->second->bodyMorph);
                for (const auto& shape : it->second->hideShapes) SetHideShape(id, shape, true);
                SetShowRealBodyOn(id, it->second->showRealBody);
            } else if (liveSettings.contains(id)) {
                SKSE::log::warn("unpublish: '{}' already has a live holder; keeping live settings", id);
            }
        }
        SetBoxArmorType(token, snap->armorType);
        SetBoxAbility(token, snap->manualAbility);
        RecallPublished(a_slot);
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* pubToken = PubTokenArmo(a_slot))
                player->RemoveItem(pubToken, 99, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        }
        g_published.erase(std::remove(g_published.begin(), g_published.end(), snap), g_published.end());
        ResetPublishedTokenState(a_slot);
        GiveOrRemoveToken(token, true);
        SaveGlobalSettings();
        return true;
    }

    void StampAllPublishTokens()
    {
        for (const auto& snap : g_published) {
            auto* token = PubTokenArmo(snap->pubSlot);
            StampSnapshotStats(*snap);
            if (!token) continue;
            token->fullName = ("Costume: " + snap->label).c_str();
            if (snap->sourceSlot >= 30 && snap->sourceSlot <= 61) {
                const auto mask = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(
                    1u << (snap->sourceSlot - 30));
                token->bipedModelData.bipedObjectSlots = mask;
                for (auto* addon : token->armorAddons)
                    if (addon) addon->bipedModelData.bipedObjectSlots = mask;
            }
        }
    }

    bool AssignNpcPersist(RE::Actor* a_actor, const std::vector<std::string>& a_contents)
    {
        if (!NpcEspLoaded() || !a_actor || a_actor == RE::PlayerCharacter::GetSingleton() ||
            a_contents.empty()) {
            return false;
        }
        if (!HasActorBindings(a_actor) &&
            InjectedNpcCount() >= static_cast<std::size_t>(g_maxNpcInjected))
            return false;
        for (const auto& item : g_nprAssignments)
            if (item.actorFormID == a_actor->GetFormID()) return false;
        std::unordered_set<int> used;
        for (const auto& item : g_nprAssignments) used.insert(item.poolSlot);
        for (const auto& item : g_unresolvedNpr) used.insert(item.poolSlot);
        int slot = -1;
        for (int i = 0; i < kPoolSize; ++i)
            if (!used.contains(i)) { slot = i; break; }
        if (slot < 0) return false;

        NprAssignmentInfo item;
        item.poolSlot = static_cast<std::uint8_t>(slot);
        item.actorFormID = a_actor->GetFormID();
        item.handle = a_actor->GetHandle();
        item.female = a_actor->GetActorBase() &&
            a_actor->GetActorBase()->GetSex() == RE::SEXES::kFemale;
        for (auto id : a_contents) {
            CanonicalizeColonId(id);
            if (id.empty() || !ResolveColonForm<RE::TESObjectARMA>(id)) return false;
            if (std::find(item.contents.begin(), item.contents.end(), id) == item.contents.end())
                item.contents.push_back(std::move(id));
        }
        if (item.contents.empty()) return false;
        auto* token = NprTokenArmo(slot);
        auto* equip = RE::ActorEquipManager::GetSingleton();
        if (!token || !equip) return false;
        g_nprAssignments.push_back(std::move(item));
        auto& saved = g_nprAssignments.back();
        if (!ActorHasItem(a_actor, token))
            a_actor->AddObjectToContainer(token, nullptr, 1, nullptr);
        equip->EquipObject(a_actor, token, nullptr, 1, nullptr, true, false, false);
        RegisterNpr(a_actor, saved);
        ReconcileActorByHandle(a_actor->GetHandle());
        SyncPersistManifest();
        return true;
    }

    bool RemoveNpcPersist(RE::Actor* a_actor)
    {
        if (!a_actor) return false;
        const auto it = std::find_if(g_nprAssignments.begin(), g_nprAssignments.end(),
            [a_actor](const NprAssignmentInfo& item) {
                return item.actorFormID == a_actor->GetFormID();
            });
        if (it == g_nprAssignments.end()) return false;
        const int slot = it->poolSlot;
        auto* token = NprTokenArmo(slot);
        g_nprAssignments.erase(it);  // erase before unequip so the sink cannot schedule a restore
        if (token) {
            RemoveActorToken(a_actor, token->GetFormID());
            if (auto* equip = RE::ActorEquipManager::GetSingleton())
                equip->UnequipObject(a_actor, token, nullptr, 1, nullptr, true, false, false);
            a_actor->RemoveItem(token, 99, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        }
        ReconcileActorByHandle(a_actor->GetHandle());
        SyncPersistManifest();
        return true;
    }

    void RestoreNpcPersistWear()
    {
        if (!NpcEspLoaded() || !CefEnabled()) return;
        auto* equip = RE::ActorEquipManager::GetSingleton();
        if (!equip) return;
        for (auto& item : g_nprAssignments) {
            auto* actor = ResolveNprActor(item);
            auto* token = NprTokenArmo(item.poolSlot);
            if (!actor || !token) continue;
            item.restoreSuspended = false;
            if (!actor->GetWornArmor(token->GetFormID())) {
                if (!ActorHasItem(actor, token))
                    actor->AddObjectToContainer(token, nullptr, 1, nullptr);
                equip->EquipObject(actor, token, nullptr, 1, nullptr, true, false, false);
            }
            RegisterNpr(actor, item);
            ReconcileActorByHandle(actor->GetHandle());
        }
    }

    std::vector<NprAssignmentInfo> NprAssignmentsSnapshot()
    {
        auto out = g_nprAssignments;
        for (const auto& saved : g_unresolvedNpr) {
            out.push_back({ saved.poolSlot, saved.actorFormID, {}, saved.contents,
                saved.female, true, false });
        }
        return out;
    }

    void OnNpcActorLoaded(RE::ActorHandle a_handle)
    {
        auto ref = a_handle.get();
        auto* actor = ref ? ref.get()->As<RE::Actor>() : nullptr;
        if (!actor || !CefEnabled()) return;
        for (auto& item : g_nprAssignments) {
            if (item.actorFormID != actor->GetFormID()) continue;
            if (!HasActorBindings(actor) &&
                InjectedNpcCount() >= static_cast<std::size_t>(g_maxNpcInjected))
                return;
            item.handle = a_handle;
            auto* token = NprTokenArmo(item.poolSlot);
            auto* equip = RE::ActorEquipManager::GetSingleton();
            if (token && equip && !actor->GetWornArmor(token->GetFormID())) {
                if (!ActorHasItem(actor, token))
                    actor->AddObjectToContainer(token, nullptr, 1, nullptr);
                equip->EquipObject(actor, token, nullptr, 1, nullptr, true, false, false);
            }
            RegisterNpr(actor, item);
            ReconcileActorByHandle(a_handle);
            return;
        }
    }

    std::vector<NprSaveAssignment> NprAssignmentsForSave()
    {
        auto out = g_unresolvedNpr;
        for (const auto& item : g_nprAssignments)
            out.push_back({ item.poolSlot, item.actorFormID, item.contents, item.female });
        return out;
    }

    void RestoreNprAssignment(std::uint8_t a_slot, RE::FormID a_actor,
        std::vector<std::string> a_contents, bool a_female)
    {
        if (a_slot >= kPoolSize || !a_actor) return;
        for (const auto& item : g_nprAssignments)
            if (item.poolSlot == a_slot || item.actorFormID == a_actor) return;
        for (auto& id : a_contents) CanonicalizeColonId(id);
        std::erase_if(a_contents, [](const std::string& id) { return id.empty(); });
        g_nprAssignments.push_back({ a_slot, a_actor, {}, std::move(a_contents), a_female });
    }

    void CarryUnresolvedNprAssignment(std::uint8_t a_slot, RE::FormID a_actor,
        std::vector<std::string> a_contents, bool a_female)
    {
        if (a_slot >= kPoolSize || !a_actor) return;
        for (const auto& item : g_unresolvedNpr)
            if (item.poolSlot == a_slot || item.actorFormID == a_actor) return;
        for (auto& id : a_contents) CanonicalizeColonId(id);
        std::erase_if(a_contents, [](const std::string& id) { return id.empty(); });
        g_unresolvedNpr.push_back({ a_slot, a_actor, std::move(a_contents), a_female });
    }

    std::size_t UnresolvedNprAssignmentCount() { return g_unresolvedNpr.size(); }

    void ReapplyNpcBindings()
    {
        InitializeNpcSupport();
        for (auto& binding : g_bindings) {
            if (!binding.wearer) continue;
            if (auto* actor = ResolveActor(binding)) {
                const bool allowed = g_hidden[binding.pubSlot] || HasActorBindings(actor) ||
                    InjectedNpcCount() < static_cast<std::size_t>(g_maxNpcInjected);
                if (allowed) {
                    // Ability spells honor the master switch on load too (§7.6):
                    // registration may proceed (visuals are cefOn-gated inside
                    // ReconcileActor), but spells must not appear while CEF is off.
                    if (const auto* snap = PubBySlot(binding.pubSlot))
                        ApplyManualAbility(actor, *snap, CefEnabled());
                    if (!g_hidden[binding.pubSlot])
                        if (const auto* snap = PubBySlot(binding.pubSlot)) RegisterSnapshot(actor, *snap);
                    ReconcileActorByHandle(actor->GetHandle());
                }
            }
        }
        RestoreNpcPersistWear();
    }

    void SyncNpcAbilities()
    {
        // Master-switch parity for NPC ability spells (§7.6): ApplyBoxAbilities
        // is player-scoped, so it calls this right after to converge every
        // publish binding - a worn token grants its spells only while CEF is
        // enabled and loses them the moment the master toggle goes off. Loaded
        // actors only; unloaded ones converge via ReapplyNpcBindings /
        // OnNpcTokenEquip when they return.
        const bool cefOn = CefEnabled();
        for (auto& binding : g_bindings) {
            const auto* snap = PubBySlot(binding.pubSlot);
            if (!snap) continue;
            if (auto* actor = ResolveActor(binding))
                ApplyManualAbility(actor, *snap, cefOn && binding.wearer);
        }
    }

    bool HasNprWork()
    {
        // Character::Load3D-thunk gate (may run during background loading):
        // emptiness read only - same threading model as the thunk's existing
        // HasActorBindings call. OnNpcActorLoaded services npc-persist
        // assignments exclusively, so their absence makes the task pointless.
        return !g_nprAssignments.empty();
    }

    void ClearNpcBindings()
    {
        for (auto& binding : g_bindings) {
            if (auto* actor = ResolveActor(binding))
                if (const auto* snap = PubBySlot(binding.pubSlot))
                    ApplyManualAbility(actor, *snap, false);
        }
        g_bindings.clear();
        g_unresolved.clear();
        g_hidden.clear();
        g_nprAssignments.clear();
        g_unresolvedNpr.clear();
    }

    std::vector<PubSaveState> PubStatesForSave()
    {
        std::vector<PubSaveState> out;
        for (const auto& snap : g_published)
            out.push_back({ static_cast<std::uint8_t>(snap->pubSlot), g_hidden[snap->pubSlot] });
        return out;
    }

    std::vector<PubBindSave> PubBindingsForSave()
    {
        std::vector<PubBindSave> out = g_unresolved;
        for (const auto& b : g_bindings)
            out.push_back({ static_cast<std::uint8_t>(b.pubSlot), b.actorFormID,
                static_cast<std::uint8_t>((b.holder ? 1 : 0) | (b.wearer ? 2 : 0)) });
        return out;
    }

    void RestorePubState(std::uint8_t a_slot, bool a_hidden)
    {
        if (a_slot < kPoolSize) g_hidden[a_slot] = a_hidden;
    }
    void RestorePubBinding(std::uint8_t a_slot, RE::FormID a_actor, std::uint8_t a_flags)
    {
        if (a_slot >= kPoolSize || !a_actor || !PubBySlot(a_slot)) return;
        g_bindings.push_back({ a_slot, a_actor, {}, (a_flags & 1) != 0, (a_flags & 2) != 0 });
    }
    void CarryUnresolvedPubBinding(std::uint8_t a_slot, RE::FormID a_actor, std::uint8_t a_flags)
    {
        if (a_slot >= kPoolSize || !a_actor) return;
        g_unresolved.push_back({ a_slot, a_actor, a_flags });
    }
    std::size_t UnresolvedPubBindingCount() { return g_unresolved.size(); }
    int MaxNpcInjected() { return g_maxNpcInjected; }

    std::vector<std::string> NpcDiagLines()
    {
        std::size_t holders = 0, wearers = 0;
        for (const auto& b : g_bindings) {
            holders += b.holder;
            wearers += b.wearer;
        }
        int resolvedTokens = 0;
        for (int i = 0; i < kPoolSize; ++i)
            if (auto* token = PubTokenArmo(i); token && IsPublishToken(token->GetFormID()))
                ++resolvedTokens;
        std::vector<std::string> out{
            "# NPC",
            std::string("addon esp: ") + (NpcEspLoaded() ? "loaded" : "NOT LOADED"),
            "published: " + std::to_string(g_published.size()) + " (dormant: " +
                std::to_string(NpcEspLoaded() ? 0 : g_published.size()) + ")",
            "bindings: " + std::to_string(wearers) + " wearer(s), " +
                std::to_string(holders) + " holder(s), " + std::to_string(g_unresolved.size()) +
                " unresolved",
            "injected: " + std::to_string(InjectedNpcCount()) + " / " +
                std::to_string(g_maxNpcInjected) + " (cap)",
            "NPC persist: " + std::to_string(g_nprAssignments.size()) + " active, " +
                std::to_string(g_unresolvedNpr.size()) + " unresolved",
            "publish token forms: " + std::to_string(resolvedTokens) + " / 8; box token pool: " +
                std::to_string(BoxCount() + static_cast<int>(FreeTokens().size()))
        };
        for (const auto& snap : g_published) {
            int slotWearers = 0;
            for (const auto& binding : g_bindings)
                if (binding.pubSlot == snap->pubSlot && binding.wearer) ++slotWearers;
            out.push_back(std::format("pub {:02} '{}': rev {}, {} wearer(s)",
                snap->pubSlot + 1, snap->label, snap->rev, slotWearers));
        }
        return out;
    }
}


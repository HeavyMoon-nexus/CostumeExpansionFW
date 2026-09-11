#include "PublishStore.h"

#include "BoxStore.h"
#include "Config.h"
#include "SkinRebind.h"

#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/I/IFormFactory.h"
#include "RE/M/Misc.h"  // RE::DebugNotification
#include "RE/S/SpellItem.h"
#include <nlohmann/json.hpp>

#include <atomic>
#include <format>

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
        // Lock-free mirror of !g_nprAssignments.empty() for the Character::Load3D
        // thunk, which can run on the background loading thread while the main
        // thread mutates the vector (NPC_AUDIT_2026-08-03 M11). Refreshed after
        // every mutation; the queued task re-checks the real state on the main
        // thread, so a stale read only costs one no-op task.
        std::atomic<bool> g_nprGate{ false };

        void RefreshNprGate()
        {
            g_nprGate.store(!g_nprAssignments.empty(), std::memory_order_relaxed);
        }
        std::unordered_set<RE::FormID> g_pubForms;
        std::unordered_set<RE::FormID> g_nprForms;
        int g_maxNpcInjected = 8;
        std::unordered_map<int, std::vector<RE::BGSKeyword*>> g_pubKeywords;
        // One ability FORM per publish slot, created once and refilled in place -
        // same contract as BoxStore's StatAbility, and for the same reason: the
        // form id is written into the save (the wearer's added-spell list), an
        // in-process load restores the ability from it, and a pointer we drop is
        // an ability nobody can ever remove again. Here the wearer is usually an
        // NPC, so it would stack on THEM (v1.6.1.1).
        struct PubAbility
        {
            RE::SpellItem* spell{ nullptr };
            bool hasEffects{ false };
            bool dirty{ true };
        };
        std::unordered_map<int, PubAbility> g_pubEnchantSpells;
        void DropPubAbility(int a_slot, PubAbility& a_ability);  // fwd (defined below)

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
            // The admission gate a box's token has had since r3, which this
            // path never ran: a blacklisted content still counted towards a
            // published costume's armor, weight and keywords (review
            // 2026-09-11 F04). StatAdmittedContents is the blacklist / capture
            // policy only - NOT the model resolution the visual path uses,
            // which asks about the PLAYER's race even when the wearer is an
            // NPC of another one (F11).
            for (const auto& id : StatAdmittedContents(a_snap.contents)) {
                auto* item = ResolveColonForm<RE::TESObjectARMO>(id);
                if (!item) continue;
                // Item-data toggles ride the global per-content settings, so a
                // published costume follows the same ON/OFF decisions.
                if (StatArmorOn(id)) armor += item->GetArmorRating() * ContentTemperMult(id);
                if (StatWeightOn(id)) weight += item->weight;
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
            // Unpublished: take the ability off its wearers and mark it stale.
            // The FORM is kept - erasing the entry would strand the ability on
            // anyone still holding it (v1.6.1.1).
            if (auto it = g_pubEnchantSpells.find(a_slot); it != g_pubEnchantSpells.end()) {
                DropPubAbility(a_slot, it->second);
                it->second.hasEffects = false;
                it->second.dirty = true;
            }
            g_hidden.erase(a_slot);
        }

        // Take a slot's ability off everyone who could hold it. The effect list
        // may not be rewritten under a live ability, and a slot that goes empty
        // must not leave its old effects applied.
        void DropPubAbility(int a_slot, PubAbility& a_ability)
        {
            if (!a_ability.spell) return;
            const std::string key = "pub:" + std::to_string(a_slot);
            for (auto& binding : g_bindings) {
                if (binding.pubSlot != a_slot) continue;
                if (auto* actor = ResolveActor(binding))
                    RevokeAbility(actor, a_ability.spell, key);
            }
            RevokeAbility(RE::PlayerCharacter::GetSingleton(), a_ability.spell, key);
        }

        // The slot's ability form, refilled from the frozen snapshot when stale.
        // Never freed and never replaced: a factory form is registered in the
        // form table, so deleting it would leave a dangling id.
        PubAbility& EnsurePubAbility(const PubSnapshot& a_snap)
        {
            auto& ability = g_pubEnchantSpells[a_snap.pubSlot];
            if (!ability.spell) {
                auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
                ability.spell = factory ? factory->Create() : nullptr;
                if (!ability.spell) return ability;
                ability.spell->data.spellType = RE::MagicSystem::SpellType::kAbility;
                ability.spell->data.castingType = RE::MagicSystem::CastingType::kConstantEffect;
                ability.spell->data.delivery = RE::MagicSystem::Delivery::kSelf;
                ability.spell->data.costOverride = 0;
                ability.dirty = true;
            }
            if (!ability.dirty) return ability;
            const std::string label = "Costume Stats: " + a_snap.label;
            ability.spell->fullName = label.c_str();
            DropPubAbility(a_snap.pubSlot, ability);
            // The SAME filler a normal box uses (review 2026-09-09 F14). The
            // rebuild that used to live here kept only {mgef, magnitude} and
            // zeroed area/duration with no conditions, so publishing an outfit
            // quietly undid the v1.6.1 conditional-enchant fix - a "while
            // sneaking" effect became always-on. It clears the old effect list
            // itself, honors the per-content enchant toggle, and skips
            // quarantined contents.
            ability.hasEffects =
                FillContentEnchantSpell(ability.spell, a_snap.contents, label.c_str());
            if (!ability.hasEffects) {
                // Nothing reachable (the contents' plugin is gone, or the global
                // capture snapshot was pruned) - fall back to the costume's own
                // frozen list so a published costume never loses its stats
                // outright. Flat by construction: this is all the snapshot has.
                // Retired, not freed: RemoveSpell only flags an active effect,
                // and its later teardown still reads the Effect*.
                ability.spell->effects.clear();
                for (const auto& [id, effects] : a_snap.enchants) {
                    if (!StatEnchantOn(id)) continue;  // item-data toggle
                    for (const auto& frozen : effects) {
                        auto* mgef = ResolveColonForm<RE::EffectSetting>(frozen.mgef);
                        if (!mgef) continue;
                        auto* effect = new RE::Effect();
                        effect->baseEffect = mgef;
                        effect->effectItem.magnitude = frozen.magnitude;
                        effect->effectItem.area = 0;
                        effect->effectItem.duration = 0;
                        ability.spell->effects.push_back(effect);
                    }
                }
                ability.hasEffects = !ability.spell->effects.empty();
            }
            ability.dirty = false;
            return ability;
        }

        void ApplyManualAbility(RE::Actor* a_actor, const PubSnapshot& a_snap, bool a_equip)
        {
            if (!a_actor) return;
            if (!a_snap.manualAbility.empty()) {
                if (auto* spell = ResolveColonForm<RE::SpellItem>(a_snap.manualAbility)) {
                    const std::string manualKey = "manual:" + a_snap.manualAbility;
                    if (a_equip) GrantAbility(a_actor, spell, manualKey);
                    else RevokeAbility(a_actor, spell, manualKey);
                }
            }
            // Always run the removal branch (as BoxStore's SyncAbility does), so a
            // wearer can never be left holding an ability CEF has stopped granting.
            auto& ability = EnsurePubAbility(a_snap);
            if (!ability.spell) return;
            const std::string key = "pub:" + std::to_string(a_snap.pubSlot);
            if (a_equip && ability.hasEffects) GrantAbility(a_actor, ability.spell, key);
            else RevokeAbility(a_actor, ability.spell, key);
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
                // Freeze the per-content settings at registration instead of the
                // null-settings live fallback (NPC_AUDIT_2026-08-03 H2). Two
                // reasons: (1) predictability - a later live-settings edit no
                // longer silently restyles an already-dressed NPC mid-session;
                // (2) hide-when-worn is deliberately NOT carried over: it is a
                // player-equipment feature ("hide my nails under my boots"), and
                // evaluating the player's hide rules against an NPC's AI-driven
                // outfit made content vanish for no visible reason - the prime
                // suspect for the owner-observed wig auto-unequip (a follower
                // whose DefaultOutfit IS its own slot-31 wig re-equips it on
                // every outfit re-evaluation, which would hide a slot-31-gated
                // CEF wig on the next Reconcile).
                auto settings = std::make_shared<ContentSettings>();
                settings->genderMode = GenderModeFor(id);
                settings->bodyMorph = BodyMorphOn(id);
                const auto hideShapes = HideShapesFor(id);
                settings->hideShapes.insert(hideShapes.begin(), hideShapes.end());
                settings->showRealBody = ShowRealBodyOn(id);
                any |= RegisterActorContent(a_actor, id, NprTokenId(a_assignment.poolSlot),
                    token->GetFormID(), std::move(settings));
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

    void MarkPublishAbilityStale(int a_pubSlot)
    {
        const auto it = g_pubEnchantSpells.find(a_pubSlot);
        if (it == g_pubEnchantSpells.end()) {
            return;  // not built yet; EnsurePubAbility will build it fresh
        }
        DropPubAbility(a_pubSlot, it->second);
        it->second.dirty = true;
    }

    int PublishedSlotHolding(const std::string& a_content)
    {
        for (const auto& snap : g_published) {
            if (snap && std::find(snap->contents.begin(), snap->contents.end(), a_content) !=
                            snap->contents.end())
                return snap->pubSlot;
        }
        return -1;
    }

    int NpcPersistSlotHolding(const std::string& a_content)
    {
        // Unresolved assignments count too: an NPC that has not loaded yet still
        // owns its costume, and its items are still in the hidden store.
        for (const auto& item : g_unresolvedNpr) {
            if (std::find(item.contents.begin(), item.contents.end(), a_content) !=
                item.contents.end())
                return item.poolSlot;
        }
        for (const auto& item : g_nprAssignments) {
            if (std::find(item.contents.begin(), item.contents.end(), a_content) !=
                item.contents.end())
                return item.poolSlot;
        }
        return -1;
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
        // Snapshots are being replaced, so every slot ability is stale - but the
        // FORMS stay, and each comes off its wearers here rather than being
        // forgotten while still applied (v1.6.1.1).
        for (auto& [slot, ability] : g_pubEnchantSpells) {
            DropPubAbility(slot, ability);
            ability.hasEffects = false;
            ability.dirty = true;
        }
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
        // CefEnabled gate (7.6 parity): a cell re-attach re-fires equip events
        // while the master switch is OFF, and this was the one ability path
        // without the gate - it silently re-granted spells SyncNpcAbilities had
        // just stripped (NPC_AUDIT_2026-08-03 M8).
        ApplyManualAbility(actor, *snap, a_equipped && allowed && CefEnabled());
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
        std::vector<RE::ActorHandle> wearers;
        for (auto& binding : g_bindings) {
            if (binding.pubSlot != a_slot || !binding.wearer) continue;
            if (auto* actor = ResolveActor(binding)) {
                equip->UnequipObject(actor, token, nullptr, 1, nullptr, true, false, false);
                wearers.push_back(actor->GetHandle());
            }
        }
        if (wearers.empty()) return;
        // plan-Y lesson (in-game proven, see BoxStore's carrier-swap post-mortem):
        // an unequip+equip pair issued together either COALESCES to a no-op
        // (same-frame applyNow) or STALLS >10s in the equip queue (queued pair) -
        // which made this Refresh a silent no-op and left NPC FSMP convergence
        // without any working driver (NPC_AUDIT_2026-08-03 F3). Split the pair
        // across frames: the unequip drains first, a fresh task re-equips.
        RunAfterDelayMs(750, [a_slot, wearers] {
            auto* token = PubTokenArmo(a_slot);
            auto* equip = RE::ActorEquipManager::GetSingleton();
            if (!token || !equip) return;
            for (const auto& handle : wearers) {
                auto ref = handle.get();
                auto* actor = ref ? ref.get()->As<RE::Actor>() : nullptr;
                if (!actor) continue;
                // The 750ms window is a real gap: a Recall/Unpublish that ran in
                // between already stripped this actor - re-equipping would undo
                // it (merge review 2026-08-04). Only re-equip a still-live wearer
                // binding.
                const bool stillBound = std::any_of(g_bindings.begin(), g_bindings.end(),
                    [&](const PubBinding& b) {
                        return b.pubSlot == a_slot && b.wearer &&
                               b.actorFormID == actor->GetFormID();
                    });
                if (!stillBound) continue;
                if (!actor->GetWornArmor(token->GetFormID()))
                    equip->EquipObject(actor, token, nullptr, 1, nullptr, true, false, false);
            }
        });
    }

    bool RefreshNpcPersist(RE::Actor* a_actor)
    {
        // The npc-persist counterpart of RefreshPubWearers - there was NO manual
        // FSMP-convergence driver for persist at all (the only re-equips were the
        // 30s auto-restore and cell reloads; NPC_AUDIT_2026-08-03 F3). Also
        // clears a 3-strike restoreSuspended park so a user action always
        // re-arms the assignment. Same split-frame cycle as RefreshPubWearers.
        if (!a_actor) return false;
        NprAssignmentInfo* item = nullptr;
        for (auto& it : g_nprAssignments) {
            if (it.actorFormID == a_actor->GetFormID()) { item = &it; break; }
        }
        if (!item) return false;
        item->restoreSuspended = false;
        auto* token = NprTokenArmo(item->poolSlot);
        auto* equip = RE::ActorEquipManager::GetSingleton();
        if (!token || !equip) return false;
        if (a_actor->GetWornArmor(token->GetFormID()))
            equip->UnequipObject(a_actor, token, nullptr, 1, nullptr, true, false, false);
        const auto handle = a_actor->GetHandle();
        const int slot = item->poolSlot;
        RunAfterDelayMs(750, [handle, slot] {
            auto ref = handle.get();
            auto* actor = ref ? ref.get()->As<RE::Actor>() : nullptr;
            if (!actor) return;
            // The 750ms window is a real gap: RemoveNpcPersist/UninstallNpcCleanup
            // may have erased the assignment (and stripped the token) in between -
            // re-adding the carrier would dress a de-assigned NPC, and worse, the
            // pool slot can be re-baked for ANOTHER actor later (merge review
            // 2026-08-04). Re-verify the assignment is still alive.
            if (!FindNpr(actor->GetFormID(), slot)) return;
            auto* token = NprTokenArmo(slot);
            auto* equip = RE::ActorEquipManager::GetSingleton();
            if (!token || !equip) return;
            if (!ActorHasItem(actor, token))
                actor->AddObjectToContainer(token, nullptr, 1, nullptr);
            if (!actor->GetWornArmor(token->GetFormID()))
                equip->EquipObject(actor, token, nullptr, 1, nullptr, true, false, false);
            ReconcileActorByHandle(handle);
        });
        return true;
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
        // The box's ORIGINAL slot may have been handed to another box while this
        // costume was published (publish frees the source token). There is one
        // token per biped slot, so the costume then has to come back somewhere
        // else - and its slot decides what it hides. Say so: silently changing
        // it read as "unpublish re-stamped my box" (test run 2026-09-10).
        const int landedSlot = TokenSlot(token);
        const bool relocated = landedSlot != snap->sourceSlot;
        if (relocated) {
            SKSE::log::warn(
                "unpublish: slot {} is taken by another box - '{}' comes back on slot {} ('{}'). "
                "Its biped slot decides what it hides, so the outfit may behave differently.",
                snap->sourceSlot, snap->label, landedSlot, token);
        }
        // PHASE 1 - validate the WHOLE restore before anything is touched
        // (review 2026-09-09 F03). This used to ignore every AddBox result and
        // then delete the snapshot regardless, so a single refused content left
        // a box missing pieces AND destroyed the definition they came from. The
        // snapshot is the only copy of the costume's composition; it is the last
        // thing to go, and only once the box that replaces it is certain.
        //
        // Two classes of problem, and they get opposite answers:
        //
        //   BLOCKING - a second owner already holds the content, or the list
        //   repeats an id. Restoring would put one content under two holders,
        //   the exact state F06 exists to prevent. Refuse; the snapshot is
        //   untouched and the user can resolve it.
        //
        //   DROPPED - the content simply cannot be re-captured (its plugin is
        //   gone, it is blacklisted now). Refusing here would be worse than the
        //   bug: UnpublishToBox is the ONLY way to free a publish slot, so a
        //   costume built on an uninstalled mod would be stuck forever, and its
        //   items stuck in the hidden store with it. Drop those ids, report
        //   them, and let the store items become orphans - the load-time sweep
        //   and the Recovery page exist precisely to hand those back.
        const std::string selfHolder = PublishHolderId(a_slot);
        std::vector<std::string> restorable;
        std::unordered_set<std::string> seen;
        for (const auto& id : snap->contents) {
            if (const std::string holder = ContentHolder(id);
                !holder.empty() && holder != selfHolder) {
                SKSE::log::warn("unpublish: slot {} kept - '{}' is also held by '{}'",
                    a_slot, id, holder);
                return false;
            }
            if (!seen.insert(id).second) {  // AddBox refuses a duplicate
                SKSE::log::warn("unpublish: slot {} kept - '{}' is listed twice", a_slot, id);
                return false;
            }
            std::string why;
            if (!CanCaptureContent(id, &why)) {  // same gate AddBox applies
                SKSE::log::warn(
                    "unpublish: slot {} - '{}' cannot go back into a box ({}); dropping it. "
                    "Its stored item is handed back by Recovery / the next load's sweep.",
                    a_slot, id, why);
                continue;
            }
            restorable.push_back(id);
        }
        // Recall BEFORE the snapshot goes: RecallPublished reads it (via PubBySlot)
        // to take the manual ability back off each wearer. It refuses up front,
        // without mutating, when the NPC add-on is not loaded - so a refusal here
        // is safe to treat as "cannot unpublish yet".
        if (!RecallPublished(a_slot)) {
            SKSE::log::warn("unpublish: slot {} kept - recall failed (NPC add-on not loaded?)", a_slot);
            return false;
        }
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* pubToken = PubTokenArmo(a_slot))
                player->RemoveItem(pubToken, 99, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        }

        // PHASE 2 - dissolve the snapshot, then rebuild the box. The erase must
        // come FIRST: ContentHolder now answers "publish:<slot>" for these ids,
        // and AddBox refuses a content that any other holder owns.
        g_published.erase(std::remove(g_published.begin(), g_published.end(), snap), g_published.end());
        bool restored = AddBox(snap->label, token, {});
        if (restored) {
            for (const auto& id : restorable) {
                if (!AddBox(snap->label, token, id)) {
                    restored = false;
                    break;
                }
                if (auto it = snap->settings.find(id); it != snap->settings.end() && it->second) {
                    SetHideSlots(id, it->second->hideSlots);
                    SetGenderMode(id, it->second->genderMode);
                    SetBodyMorphOn(id, it->second->bodyMorph);
                    for (const auto& shape : it->second->hideShapes) SetHideShape(id, shape, true);
                    SetShowRealBodyOn(id, it->second->showRealBody);
                }
            }
        }
        if (!restored) {
            // Phase 1 makes this unreachable in practice; if it happens anyway,
            // put the costume back rather than leaving a half-filled box and no
            // definition to rebuild it from. The wearers are already recalled -
            // the tokens have to be handed out again - but the composition lives.
            RemoveBox(token);
            g_published.push_back(snap);
            SaveGlobalSettings();
            SKSE::log::error(
                "unpublish: slot {} restore FAILED after validation - the published costume is "
                "kept and the partial box was rolled back; re-issue its tokens", a_slot);
            return false;
        }
        SetBoxArmorType(token, snap->armorType);
        SetBoxAbility(token, snap->manualAbility);
        ResetPublishedTokenState(a_slot);
        GiveOrRemoveToken(token, true);
        // AddBox only moves DEFINITIONS ("caller registers + reconciles"), and
        // Publish detached these contents with DetachSkinned - so without this
        // the restored box equips to nothing until the next settings reload
        // ("works after a reload" - review F08). Same post-capture order the
        // preset-assign path uses.
        for (const auto& id : restorable) {
            RegisterBoxById(id, token);
        }
        Reconcile();
        RebuildBoxAbility(token);
        ApplyBoxAbilities();
        RefreshWornToken(token);
        SaveGlobalSettings();
        SKSE::log::info("unpublish: slot {} -> box '{}' ({}/{} content(s) restored)",
            a_slot, token, restorable.size(), snap->contents.size());
        if (relocated) {
            RE::DebugNotification(std::format(
                "CostumeFW: slot {} was taken - '{}' came back on slot {}",
                snap->sourceSlot, snap->label, landedSlot).c_str());
        }
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
            // ARMA or ARMO: the persist catalog stores standard captures as ARMO
            // colon-ids, so an ARMA-only resolve silently refused every normal
            // catalog entry (NPC_AUDIT_2026-08-03 F2). CanResolveContent is the
            // same ARMA/ARMO gate the registration path applies (IMPL 10.3).
            if (id.empty() || !CanResolveContent(id)) return false;
            if (std::find(item.contents.begin(), item.contents.end(), id) == item.contents.end())
                item.contents.push_back(std::move(id));
        }
        if (item.contents.empty()) return false;
        auto* token = NprTokenArmo(slot);
        auto* equip = RE::ActorEquipManager::GetSingleton();
        if (!token || !equip) return false;
        g_nprAssignments.push_back(std::move(item));
        RefreshNprGate();
        auto& saved = g_nprAssignments.back();
        if (!ActorHasItem(a_actor, token))
            a_actor->AddObjectToContainer(token, nullptr, 1, nullptr);
        equip->EquipObject(a_actor, token, nullptr, 1, nullptr, true, false, false);
        RegisterNpr(a_actor, saved);
        ReconcileActorByHandle(a_actor->GetHandle());
        SyncPersistManifest();
        return true;
    }

    void UninstallNpcCleanup()
    {
        // "Prepare for uninstall" left every NPC dressed: tokens in NPC
        // inventories, NPR carriers equipped, ability spells granted - the
        // cleanup flow was player-only (NPC_AUDIT_2026-08-03 M9). Recall every
        // published slot (returns tokens to the player, strips spells, clears
        // bindings) and remove every npc-persist assignment; unresolvable
        // assignments (actor unloaded/gone) are dropped so nothing rides the
        // next co-save of a mod the user is about to delete.
        if (!NpcEspLoaded()) {
            return;
        }
        for (int slot = 0; slot < kPoolSize; ++slot) {
            if (PubBySlot(slot)) {
                RecallPublished(slot);
            }
        }
        // Copy first: RemoveNpcPersist erases from g_nprAssignments.
        std::vector<RE::FormID> actors;
        actors.reserve(g_nprAssignments.size());
        for (const auto& item : g_nprAssignments) {
            actors.push_back(item.actorFormID);
        }
        for (const auto id : actors) {
            auto* form = RE::TESForm::LookupByID(id);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (actor) {
                RemoveNpcPersist(actor);
            }
        }
        const auto dropped = g_nprAssignments.size() + g_unresolvedNpr.size();
        if (dropped) {
            SKSE::log::warn(
                "uninstall cleanup: dropping {} npc-persist assignment(s) whose actor "
                "could not be resolved (unloaded or gone) - their carrier tokens stay "
                "in those NPCs' inventories", dropped);
        }
        g_nprAssignments.clear();
        RefreshNprGate();
        g_unresolvedNpr.clear();
        SyncNpcAbilities();
        SyncPersistManifest();
    }

    bool UpdateNpcPersist(RE::Actor* a_actor, const std::vector<std::string>& a_contents)
    {
        // In-place contents edit for an existing assignment (SMF NPC page):
        // keeps the pool slot, unregisters contents that leave, re-registers the
        // new set (re-freezing the per-content settings snapshot, H2), re-bakes
        // the carrier via the manifest sync, and runs the split-frame re-equip
        // so FSMP converges on the new bake. An empty selection is refused -
        // removing everything is the Remove-assignment action, not an edit.
        if (!NpcEspLoaded() || !a_actor || a_contents.empty()) return false;
        NprAssignmentInfo* item = nullptr;
        for (auto& it : g_nprAssignments) {
            if (it.actorFormID == a_actor->GetFormID()) { item = &it; break; }
        }
        if (!item) return false;
        std::vector<std::string> next;
        for (auto id : a_contents) {
            CanonicalizeColonId(id);
            if (id.empty() || !CanResolveContent(id)) return false;
            if (std::find(next.begin(), next.end(), id) == next.end())
                next.push_back(std::move(id));
        }
        if (next.empty()) return false;
        for (const auto& id : item->contents) {
            if (std::find(next.begin(), next.end(), id) == next.end())
                RemoveActorContent(a_actor, id);
        }
        item->contents = std::move(next);
        item->female = a_actor->GetActorBase() &&
            a_actor->GetActorBase()->GetSex() == RE::SEXES::kFemale;
        RegisterNpr(a_actor, *item);
        SyncPersistManifest();
        RefreshNpcPersist(a_actor);
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
        RefreshNprGate();
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
        RefreshNprGate();
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
        // Addon removed mid-save: the engine already silently deleted the token
        // items, so re-applying bindings could only re-grant ability spells with
        // no visible costume and no way to unequip (NPC_AUDIT_2026-08-03 M8).
        // Cosave data is preserved either way; nothing is lost by waiting.
        if (!NpcEspLoaded()) {
            return;
        }
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
        // Character::Load3D-thunk gate (may run during background loading): an
        // atomic mirror, NOT the vector itself - reading a std::vector while the
        // main thread push_backs/erases is a data race (M11). OnNpcActorLoaded
        // services npc-persist assignments exclusively, so their absence makes
        // the task pointless.
        return g_nprGate.load(std::memory_order_relaxed);
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
        RefreshNprGate();
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


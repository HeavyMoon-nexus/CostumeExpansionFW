# FSMP external-node API patch notes

These files are draft patches/notes for DaymareOn/hdtSMP64, prepared from the
v4.0.0 source tree.

## 0001-external-node-api-entrypoints-v4.0.0.patch

Small API-surface patch only:

- bumps `PluginInterface::INTERFACE_VERSION` from `2.0.0` to `2.1.0`
- adds:
  - `attachPhysics(RE::Actor*, RE::NiAVObject*, const char*)`
  - `detachPhysics(RE::NiAVObject*)`
- implements them as safe stubs (`attachPhysics` returns `false`,
  `detachPhysics` is a no-op)

This is not enough to animate CEF cloth. It is useful as a minimal, reviewable
ABI proposal: consumers can call the method and fall back to carrier/head-part
paths when it returns `false`.

## Follow-up shape for a working patch

A functional patch should be kept parallel to existing armor/head-part logic:

1. Add `Skeleton::ExternalItem : PhysicsItem` with:
   - `RE::NiPointer<RE::NiAVObject> node`
   - `std::string prefix`
   - `std::unordered_map<RE::BSFixedString, RE::BSFixedString> renameMap`

2. Add `std::vector<ExternalItem> externalItems` to `ActorManager::Skeleton`.

3. Route `PluginInterfaceImpl::attachPhysics` to a new public
   `ActorManager::attachExternalPhysics(actor, node, physicsXmlPath)`.

4. Under the ActorManager lock:
   - locate `get3rdPersonSkeleton(actor)`
   - require `node->AsNode()` for the first implementation
   - scan the physics file with `DefaultBBP::instance()->scanBBP(rootNode)`
   - if `physicsXmlPath` is non-empty, override `physicsFile.first`
   - merge external custom bones into `npc` with a new external prefix
   - call `SkyrimSystemCreator().createOrUpdateSystem(...)`
   - store the system with `ExternalItem::setPhysics(system, isActive)`

5. Include external items in:
   - `Skeleton::reloadMeshes`
   - `Skeleton::softReloadMeshes`
   - `Skeleton::updateAttachedState`
   - `Skeleton::updateWindFactor`
   - `Skeleton::checkPhysics`
   - dead-node cleanup (`node == nullptr || node->parent == nullptr`)

6. Make `detachPhysics(node)` clear the physics system, clean the merged skeleton
   nodes for that external prefix, and erase the matching `ExternalItem`.

## Main design risk

Equipped armor gets two phases: `addArmor()` merges/renames the source skeleton
tree, then `attachArmor()` builds the system against the attached model. An
external-node implementation needs the same idea.

For CEF specifically, the currently injected holder contains bare geometry after
CEF strips the source bone tree. A future direct-FSMP backend should either:

- pass a physics source node that still contains the source bone tree and HDT
  extra data, then bind the visible CEF holder to the generated renamed bones, or
- extend FSMP's system creator so mesh skin bones can resolve through `renameMap`
  without mutating/renaming the caller-owned source node.

The first route is less invasive for FSMP and fits CEF's existing rebind/watchdog
logic. The generated prefix should probably be `External`, which means CEF's
renamed-bone parser must accept `hdtSSEPhysics_AutoRename_External_<id> <bone>`
in addition to `Armor` and `Head`.

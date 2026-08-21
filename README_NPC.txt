Costume Expansion FW - NPC Token Add-on

Install this archive alongside a core Costume Expansion FW of the same
version or newer. A release that changes nothing in the add-on leaves its
file alone, so an add-on version lower than the core's is expected and fine.
Enable CostumeFW_NPC.esp. It is a permanent separate ESL-flagged add-on and
must never be merged into CostumeFW.esp.

The shipped ESP is generated and structurally verified from the core carrier
template by tools/build_npc_addon.ps1. Its only master is Skyrim.esm; its local
FormIDs are part of the co-save/runtime ABI and must not be compacted or
renumbered.

The NPC page is available in SKSE Menu Framework. Publish freezes a box's
contents and settings into a distributable token. Refresh is the explicit
FSMP convergence action. Recall cannot recover copies held by unresolved or
deleted actors; review the NPC page before unpublishing a slot.


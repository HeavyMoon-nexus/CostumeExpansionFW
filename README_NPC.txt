Costume Expansion FW - NPC Token Add-on

This comes from the "NPC distribution" option in the installer. Up to 1.6.2 it
was a separate download; if you still have that mod installed, remove it and
tick the option instead. The ESP is the same file it has always been, so
costumes your NPCs are already wearing keep working across the change.

Enable CostumeFW_NPC.esp. It is ESL-flagged and costs no load-order slot. It is
a permanent separate add-on and must never be merged into CostumeFW.esp.

The shipped ESP is generated and structurally verified from the core carrier
template by tools/build_npc_addon.ps1. Its only master is Skyrim.esm; its local
FormIDs are part of the co-save/runtime ABI and must not be compacted or
renumbered.

The NPC page lives in SKSE Menu Framework; the MCM only has a stub that points
at it. Publish freezes a box's contents and settings into a distributable
token. From 1.6.4 it no longer gives the box's slot away: the token is held in
reserve, so unpublishing puts the costume back on the token and the biped slot
it came from, or refuses and tells you which of the three reasons it is.
Refresh is the explicit FSMP convergence action.

Recall and Unpublish are different operations. Recall pulls the token and its
ability back from everyone wearing the costume and clears the bindings; the
costume itself stays on the NPC page, the way a box stays in the box list.
Unpublish dissolves it back into a box, and only once every item is back where
it belongs. Recall cannot reach copies held by actors that are unloaded or no
longer in the save, so review the NPC page before you unpublish a slot.

Two things worth knowing:

* Published costumes and NPC-persist items build their physics carrier from the
  PLAYER's race. A costume that ships a separate mesh per race can look wrong on
  an NPC of another race. Boxes on your own character are not affected.
* "Prepare for uninstall" recalls every published costume, clears every NPC
  persist assignment, and gives back the items inside published costumes. Up to
  1.6.2 it did none of that and still told you it was safe to remove the mod, so
  on those versions unpublish your costumes back into their boxes first.

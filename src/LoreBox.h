#pragma once

namespace CostumeFW
{
    // LoreBox ("Item and Spell Tooltips") integration (soft dependency).
    //
    // LoreBox shows a tooltip for an inventory item built from its keywords whose
    // editorID starts with "LoreBox_": for each, it asks the GFx translator for
    // "$<keyword>" and displays the result if it translated to something. We ship a
    // KID ini (CostumeFW_KID.ini) that puts a unique keyword
    // "LoreBox_CEFBox<slot>" on each Costume Box token, then hook
    // BSScaleformTranslator::Translate so those keys resolve to the box's CURRENT
    // contents at translate time (always live). All other keys pass through to the
    // game translator untouched. If LoreBox is absent, nothing ever requests these
    // keys, so the hook is inert - a clean soft dependency.
    //
    // Installs a class-vtable swap on BSScaleformTranslator (vfunc 0x2), like the
    // Load3D hook. Call once on kDataLoaded. Main thread.
    void InstallLoreBoxHook();
}

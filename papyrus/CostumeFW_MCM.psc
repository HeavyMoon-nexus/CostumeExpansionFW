Scriptname CostumeFW_MCM extends SKI_ConfigBase

; Costume Expansion FW - SkyUI MCM.
; Drives the CFW_Native Papyrus API (backed by CostumeExpansionFW.dll).
;   Main    - master on/off, dependency status, version.
;   Persist - always-on accessories (worn-capture; no slot / no token gate).
;   Boxes   - per-slot boxes (token-gated contents); capture, armor type, preset
;             assign/export, delete.
;   Presets - distributable CEFP_*.json content sets; assign to a box.
; Native mutators are deferred to the main thread, so we set option values to the
; user's intent and reconcile the display on the next ForcePageReset.

int property CURRENT_VERSION = 17 autoReadonly

; --- Main page state ---
int _optEnable
int _optReload
int _optUninstall

; --- Persist page state ---
int      _optAddPersist
int      _optAddInvPersist  ; "+ Add from inventory" (no equip needed)
int      _optRemoveAllPersist
int      _persistPresetOpt   ; persist "Preset" menu (assign/clear)
int      _persistExportOpt   ; persist "Export as preset" input
int[]    _persistRemoveOpts
int[]    _persistHideOpts
string[] _persistRemoveContents
string[] _persistWornIdsCache
int[]    _persistActiveOpts   ; per-catalog-entry "active on this save" toggle (M2)
int[]    _persistMorphOpts    ; per-catalog-entry body-morph toggle
int[]    _persistUncatOpts    ; active-but-not-in-catalog [deactivate] rows (M2)
string[] _persistUncatIds

; --- Boxes page state ---
; Each box now has its OWN page (scroll-limit fix); "Boxes" is a short overview.
int      _curBoxIndex       ; the box rendered on the current single-box page
string[] _boxPageNames      ; page name per box page, for OnPageReset lookup
int[]    _boxPageSlots      ; the biped slot each box page maps to (parallel)
int _optNewBox
int[]    _equipOpts
int[]    _equipBoxIdx
int[]    _addWornOpts
int[]    _addWornBoxIdx
int[]    _addInvOpts        ; "+ Add from inventory" (single box page)
int[]    _addInvBoxIdx
int[]    _deleteOpts
string[] _deleteTokens
int[]    _removeOpts
int[]    _hideOpts
int[]    _morphOpts          ; per-content body-morph toggle (single box page)
string[] _removeTokens
string[] _removeContents
int[]    _distribOpts
string[] _distribTokens
int[]    _armorTypeOpts
string[] _armorTypeTokens
int[]    _assignOpts        ; per-box "Assign preset" menu
string[] _assignTokens
int[]    _exportOpts        ; per-box "Export as preset" input
string[] _exportTokens
string[] _wornIdsCache      ; ids parallel to the open "add worn item" menu
int[]    _wornGenderCache   ; gender mode (0/1/2) parallel to _wornIdsCache
string[] _freeTokenIds      ; ids parallel to the open "new box" slot menu
string[] _presetFilesCache  ; files parallel to the open "assign preset" menu

; --- Presets page state ---
int[]    _presetAssignOpts  ; per-preset "Assign to box" menu
string[] _presetAssignFiles
string[] _presetBoxTokens   ; tokens parallel to the open box-picker menu

; Hidden holding container (CFW_Storage 00080D) that preserves captured items
; WITH their instance data (tempering / player enchant). Lazily created.
ObjectReference _store

event OnConfigInit()
    SetupConfig()
endEvent

; SkyUI calls OnConfigInit only on FIRST registration. On a version bump it calls
; OnVersionUpdate instead - so rebuild ModName + Pages here too, otherwise existing
; saves keep the old page layout (new pages never appear without a new game).
event OnVersionUpdate(int a_version)
    if a_version >= 9
        SetupConfig()
    endIf
endEvent

function SetupConfig()
    ModName = "Costume Expansion FW"
    _curBoxIndex = 0
    _boxPageNames = new string[1]
    _boxPageSlots = new int[1]
    BuildPages()  ; Main, Persist, Boxes (overview), one page per box, Presets

    ; Init every option-map array so a Find() before a page is built is safe.
    _persistPresetOpt = -1
    _persistExportOpt = -1
    _optAddInvPersist = -1
    _persistRemoveOpts     = new int[1]
    _persistHideOpts       = new int[1]
    _persistRemoveContents = new string[1]
    _persistWornIdsCache   = new string[1]
    _persistActiveOpts     = new int[1]
    _persistMorphOpts      = new int[1]
    _persistUncatOpts      = new int[1]
    _persistUncatIds       = new string[1]
    _equipOpts     = new int[1]
    _equipBoxIdx   = new int[1]
    _addWornOpts   = new int[1]
    _addWornBoxIdx = new int[1]
    _addInvOpts    = new int[1]
    _addInvBoxIdx  = new int[1]
    _deleteOpts    = new int[1]
    _deleteTokens  = new string[1]
    _removeOpts     = new int[1]
    _hideOpts       = new int[1]
    _morphOpts      = new int[1]
    _removeTokens   = new string[1]
    _removeContents = new string[1]
    _distribOpts    = new int[1]
    _distribTokens  = new string[1]
    _armorTypeOpts   = new int[1]
    _armorTypeTokens = new string[1]
    _assignOpts    = new int[1]
    _assignTokens  = new string[1]
    _exportOpts    = new int[1]
    _exportTokens  = new string[1]
    _wornIdsCache    = new string[1]
    _wornGenderCache = new int[1]
    _freeTokenIds    = new string[1]
    _presetFilesCache = new string[1]
    _presetAssignOpts = new int[1]
    _presetAssignFiles = new string[1]
    _presetBoxTokens = new string[1]
endFunction

int function GetVersion()
    return CURRENT_VERSION
endFunction

; Rebuild the page list to one page per box (avoids SkyUI's per-page option/scroll
; limit). Called on config init/version-update AND each time the menu opens, so the
; left-hand page list reflects the current boxes (a box added this session appears
; after closing + reopening the MCM).
function BuildPages()
    int n = CFW_Native.GetBoxCount()
    string[] p = Utility.CreateStringArray(n + 5, "")
    p[0] = "Main"
    p[1] = "Persist"
    p[2] = "Boxes"
    _boxPageNames = Utility.CreateStringArray(n + 1, "")  ; +1 so size is never 0
    _boxPageSlots = Utility.CreateIntArray(n + 1, 0)
    int i = 0
    while i < n
        int slot = CFW_Native.GetTokenSlot(CFW_Native.GetBoxToken(i))
        string pname = "Box " + slot + ": " + SlotName(slot)
        p[3 + i] = pname
        _boxPageNames[i] = pname
        _boxPageSlots[i] = slot
        i += 1
    endWhile
    p[3 + n] = "Presets"
    p[4 + n] = "Diagnostics"
    Pages = p
endFunction

event OnConfigOpen()
    BuildPages()
endEvent

event OnPageReset(string a_page)
    if a_page == "Boxes"
        ResetBoxesOverviewPage()
    elseIf a_page == "Persist"
        ResetPersistPage()
    elseIf a_page == "Presets"
        ResetPresetsPage()
    elseIf a_page == "Diagnostics"
        ResetDiagnosticsPage()
    elseIf a_page == "Main" || a_page == ""
        ResetMainPage()
    else
        ; Box page: resolve by SLOT (stable) not by page position, so a deletion
        ; that shifts box indices still maps each page to the right box.
        int pi = _boxPageNames.Find(a_page)
        if pi < 0
            ResetMainPage()
            return
        endIf
        int boxIdx = CFW_Native.GetBoxBySlot(_boxPageSlots[pi])
        if boxIdx >= 0
            ResetSingleBoxPage(boxIdx)
        else
            ResetDeletedBoxPage()  ; box was deleted this session
        endIf
    endIf
endEvent

string Function ArmorTypeName(int a_type)
    if a_type == 1
        return "Light Armor"
    elseIf a_type == 2
        return "Heavy Armor"
    endIf
    return "Clothing"
endFunction

string Function SlotName(int a_slot)
    if a_slot == 31
        return "Hair (Wig)"
    elseIf a_slot == 44
        return "Face/Eyes"
    elseIf a_slot == 45
        return "Neck"
    elseIf a_slot == 46
        return "Chest"
    elseIf a_slot == 47
        return "Cloak"
    elseIf a_slot == 48
        return "Belly/Garter"
    elseIf a_slot == 49
        return "Skirts/Pants"
    elseIf a_slot == 52
        return "Underwear"
    elseIf a_slot == 53
        return "Leg Upper/R"
    elseIf a_slot == 54
        return "Leg Lower/L"
    elseIf a_slot == 55
        return "Face Mask"
    elseIf a_slot == 56
        return "Bra/Chest2"
    elseIf a_slot == 57
        return "Shoulder"
    elseIf a_slot == 58
        return "Arm Upper/R"
    elseIf a_slot == 59
        return "Arm Lower/L"
    elseIf a_slot == 60
        return "Misc"
    endIf
    return "slot " + a_slot
endFunction

; Comma-joined in-game names of a box's packed contents (for the info panel).
string Function BoxContentsSummary(int a_boxIndex)
    string[] contents = CFW_Native.GetBoxContents(a_boxIndex)
    if contents.Length == 0
        return "(empty)"
    endIf
    string out = ""
    int ci = 0
    while ci < contents.Length
        if ci > 0
            out += ", "
        endIf
        out += CFW_Native.GetItemName(contents[ci])
        ci += 1
    endWhile
    return out
endFunction

; If the captured item has attached scripts, warn that its script-driven behavior
; won't run under CEF (the mesh is injected but the item is stored, not worn).
function WarnIfScripted(string contentId)
    if CFW_Native.ContentHasScript(contentId)
        Debug.MessageBox("CostumeFW: '" + CFW_Native.GetItemName(contentId) + "' has attached scripts. Its equip- or possession-driven behavior may not work when captured (CEF injects the mesh, but the item is stored - not worn). Enchantment effects and keywords are still applied.")
    endIf
endFunction

; -----------------------------------------------------------------------------
; Main page
; -----------------------------------------------------------------------------
function ResetMainPage()
    SetCursorFillMode(TOP_TO_BOTTOM)

    AddHeaderOption("Costume Expansion FW")
    _optEnable = AddToggleOption("Enable CEF", CFW_Native.IsEnabled())
    _optReload = AddTextOption("Reload settings from disk", "")

    AddHeaderOption("Dependencies")
    string skee = "MISSING"
    if CFW_Native.IsBodyMorphReady()
        skee = "OK"
    endIf
    AddTextOption("RaceMenu / skee (body morph)", skee, OPTION_FLAG_DISABLED)
    string boxesEsp = "MISSING"
    if Game.GetFormFromFile(0x00080D, "CostumeFW.esp")
        boxesEsp = "OK"
    endIf
    AddTextOption("CostumeFW.esp", boxesEsp, OPTION_FLAG_DISABLED)

    AddHeaderOption("Maintenance")
    _optUninstall = AddTextOption("Prepare for uninstall", "")

    AddHeaderOption("About")
    AddTextOption("Version", "" + CURRENT_VERSION, OPTION_FLAG_DISABLED)
endFunction

; -----------------------------------------------------------------------------
; Persist page (always-on accessories; worn capture, no token gate)
; -----------------------------------------------------------------------------
function ResetPersistPage()
    SetCursorFillMode(TOP_TO_BOTTOM)

    _optAddPersist = AddMenuOption("+ Add worn item", "")
    _optAddInvPersist = AddMenuOption("+ Add from inventory", "")

    string ppreset = CFW_Native.GetPersistPreset()
    if ppreset == ""
        ppreset = "(manual)"
    endIf
    _persistPresetOpt = AddMenuOption("Preset", ppreset)
    _persistExportOpt = AddInputOption("Export as preset", "")

    ; M2 (CEF_STATE_SCOPE.md): the catalog is GLOBAL (shared by every save /
    ; character); the toggle on each row is what THIS save actually shows.
    string[] contents = CFW_Native.GetPersistContents()
    AddHeaderOption("Catalog (" + contents.Length + ") - shared across saves")
    if contents.Length == 0
        AddTextOption("(none - use + Add worn item)", "", OPTION_FLAG_DISABLED)
        _persistActiveOpts     = new int[1]
        _persistMorphOpts      = new int[1]
        _persistRemoveOpts     = new int[1]
        _persistHideOpts       = new int[1]
        _persistRemoveContents = new string[1]
    else
        _persistActiveOpts     = Utility.CreateIntArray(contents.Length, -1)
        _persistMorphOpts      = Utility.CreateIntArray(contents.Length, -1)
        _persistRemoveOpts     = Utility.CreateIntArray(contents.Length, -1)
        _persistHideOpts       = Utility.CreateIntArray(contents.Length, -1)
        _persistRemoveContents = Utility.CreateStringArray(contents.Length, "")
        int i = 0
        while i < contents.Length
            _persistActiveOpts[i] = AddToggleOption(CFW_Native.GetItemName(contents[i]), CFW_Native.IsPersistActive(contents[i]))
            _persistMorphOpts[i] = AddToggleOption("  Body morph (BodySlide mesh)", CFW_Native.GetBodyMorph(contents[i]))
            _persistHideOpts[i] = AddInputOption("  Hide when worn (slots)", CFW_Native.GetHideSlots(contents[i]))
            _persistRemoveOpts[i] = AddTextOption("  Remove from catalog", "[remove]")
            _persistRemoveContents[i] = contents[i]
            i += 1
        endWhile
        AddHeaderOption("Actions")
        _optRemoveAllPersist = AddTextOption("Remove all persist", "")
    endIf

    ; Active on this save but no longer in the shared catalog (another character
    ; removed the entry). Deactivating returns the item.
    string[] act = CFW_Native.GetPersistActive()
    int uncat = 0
    int ai = 0
    while ai < act.Length
        if contents.Find(act[ai]) < 0
            uncat += 1
        endIf
        ai += 1
    endWhile
    if uncat > 0
        AddHeaderOption("Active but not in catalog (" + uncat + ")")
        _persistUncatOpts = Utility.CreateIntArray(uncat, -1)
        _persistUncatIds  = Utility.CreateStringArray(uncat, "")
        int uidx = 0
        ai = 0
        while ai < act.Length
            if contents.Find(act[ai]) < 0
                _persistUncatOpts[uidx] = AddTextOption(CFW_Native.GetItemName(act[ai]), "[deactivate]")
                _persistUncatIds[uidx] = act[ai]
                uidx += 1
            endIf
            ai += 1
        endWhile
    else
        _persistUncatOpts = new int[1]
        _persistUncatIds  = new string[1]
    endIf
endFunction

; -----------------------------------------------------------------------------
; Boxes page
; -----------------------------------------------------------------------------
; "Boxes" overview: + New box and a short read-only summary line per box. The
; full per-box controls live on each box's own page (left list) to stay under
; SkyUI's per-page option/scroll limit.
function ResetBoxesOverviewPage()
    SetCursorFillMode(TOP_TO_BOTTOM)

    int n = CFW_Native.GetBoxCount()
    _optNewBox = AddMenuOption("+ New box (pick slot)", "")
    AddHeaderOption("Boxes (" + n + ")")

    if n == 0
        AddTextOption("(none yet - use + New box)", "", OPTION_FLAG_DISABLED)
        return
    endIf
    AddTextOption("(each box has its own page in the list on the left)", "", OPTION_FLAG_DISABLED)
    int i = 0
    while i < n
        int slot = CFW_Native.GetTokenSlot(CFW_Native.GetBoxToken(i))
        int cnt = CFW_Native.GetBoxContents(i).Length
        string worn = "off"
        if CFW_Native.IsBoxWorn(i)
            worn = "WORN"
        endIf
        AddTextOption("Box " + slot + ": " + SlotName(slot) + " (" + cnt + " items)", worn, OPTION_FLAG_DISABLED)
        i += 1
    endWhile
endFunction

; Shown when a box page's box was deleted this session (the page list only
; refreshes on reopen). Avoids rendering a shifted, wrong box on that page.
function ResetDeletedBoxPage()
    SetCursorFillMode(TOP_TO_BOTTOM)
    AddHeaderOption("(box deleted)")
    AddTextOption("This box was deleted. Reopen the MCM to refresh the page list.", "", OPTION_FLAG_DISABLED)
endFunction

; Full controls for ONE box (its own page). The box index is tracked in
; _curBoxIndex; every per-box control array holds a single entry (index 0), and
; handlers that need the box index use _curBoxIndex.
function ResetSingleBoxPage(int a_idx)
    SetCursorFillMode(TOP_TO_BOTTOM)
    _curBoxIndex = a_idx

    string token = CFW_Native.GetBoxToken(a_idx)
    int slot = CFW_Native.GetTokenSlot(token)
    string[] contents = CFW_Native.GetBoxContents(a_idx)
    int cn = contents.Length
    if cn == 0
        cn = 1
    endIf

    _equipOpts     = Utility.CreateIntArray(1, -1)
    _equipBoxIdx   = Utility.CreateIntArray(1, a_idx)
    _addWornOpts   = Utility.CreateIntArray(1, -1)
    _addWornBoxIdx = Utility.CreateIntArray(1, a_idx)
    _addInvOpts    = Utility.CreateIntArray(1, -1)
    _addInvBoxIdx  = Utility.CreateIntArray(1, a_idx)
    _distribOpts   = Utility.CreateIntArray(1, -1)
    _distribTokens = Utility.CreateStringArray(1, token)
    _armorTypeOpts   = Utility.CreateIntArray(1, -1)
    _armorTypeTokens = Utility.CreateStringArray(1, token)
    _assignOpts    = Utility.CreateIntArray(1, -1)
    _assignTokens  = Utility.CreateStringArray(1, token)
    _exportOpts    = Utility.CreateIntArray(1, -1)
    _exportTokens  = Utility.CreateStringArray(1, token)
    _deleteOpts    = Utility.CreateIntArray(1, -1)
    _deleteTokens  = Utility.CreateStringArray(1, token)
    _removeOpts     = Utility.CreateIntArray(cn, -1)
    _hideOpts       = Utility.CreateIntArray(cn, -1)
    _morphOpts      = Utility.CreateIntArray(cn, -1)
    _removeTokens   = Utility.CreateStringArray(cn, "")
    _removeContents = Utility.CreateStringArray(cn, "")

    AddHeaderOption(CFW_Native.GetItemName(token) + ": " + SlotName(slot))
    _distribOpts[0] = AddToggleOption("Distribute token", CFW_Native.GetBoxEnabled(a_idx))
    _equipOpts[0] = AddToggleOption("Wear (show contents)", CFW_Native.IsBoxWorn(a_idx))
    _addWornOpts[0] = AddMenuOption("+ Add worn item", "")
    _addInvOpts[0] = AddMenuOption("+ Add from inventory", "")
    _armorTypeOpts[0] = AddMenuOption("Armor type", ArmorTypeName(CFW_Native.GetBoxArmorType(a_idx)))
    string presetName = CFW_Native.GetBoxPreset(token)
    if presetName == ""
        presetName = "(manual)"
    endIf
    _assignOpts[0] = AddMenuOption("Preset", presetName)
    _exportOpts[0] = AddInputOption("Export as preset", "")
    AddTextOption("Stats", CFW_Native.GetBoxStats(a_idx), OPTION_FLAG_DISABLED)
    _deleteOpts[0] = AddTextOption("Delete box", "")

    AddHeaderOption("Contents (" + contents.Length + ")")
    int ci = 0
    while ci < contents.Length
        _removeOpts[ci] = AddTextOption("  " + CFW_Native.GetItemName(contents[ci]), "[remove]")
        _morphOpts[ci] = AddToggleOption("  Body morph (BodySlide mesh)", CFW_Native.GetBodyMorph(contents[ci]))
        _hideOpts[ci] = AddInputOption("  Hide when worn (slots)", CFW_Native.GetHideSlots(contents[ci]))
        _removeTokens[ci] = token
        _removeContents[ci] = contents[ci]
        ci += 1
    endWhile
endFunction

; -----------------------------------------------------------------------------
; Presets page
; -----------------------------------------------------------------------------
function ResetPresetsPage()
    SetCursorFillMode(TOP_TO_BOTTOM)

    string[] names = CFW_Native.GetPresetNames()
    string[] files = CFW_Native.GetPresetFiles()
    AddHeaderOption("Presets (" + names.Length + ")")
    if names.Length == 0
        AddTextOption("(none - export a box as a preset, or install a CEFP_*.json)", "", OPTION_FLAG_DISABLED)
        _presetAssignOpts  = new int[1]
        _presetAssignFiles = new string[1]
        return
    endIf
    _presetAssignOpts  = Utility.CreateIntArray(names.Length, -1)
    _presetAssignFiles = Utility.CreateStringArray(names.Length, "")
    int i = 0
    while i < names.Length
        string assignedTo = CFW_Native.GetPresetAssignedTo(names[i])
        string stat = "free"
        if assignedTo != ""
            stat = "assigned"
        endIf
        _presetAssignOpts[i] = AddMenuOption(names[i], stat)
        _presetAssignFiles[i] = files[i]
        i += 1
    endWhile
endFunction

; -----------------------------------------------------------------------------
; Diagnostics page (read-only; lines composed natively so console + MCM agree)
; -----------------------------------------------------------------------------
function ResetDiagnosticsPage()
    SetCursorFillMode(TOP_TO_BOTTOM)
    string[] lines = CFW_Native.GetDiagLines()
    int i = 0
    while i < lines.Length
        string l = lines[i]
        if StringUtil.Find(l, "# ") == 0
            AddHeaderOption(StringUtil.Substring(l, 2))
        else
            AddTextOption(l, "", OPTION_FLAG_DISABLED)
        endIf
        i += 1
    endWhile
    AddEmptyOption()
    AddTextOption("(reopen this page to refresh)", "", OPTION_FLAG_DISABLED)
endFunction

; Per-content body-morph toggle (default OFF). Body morph is only for BodySlide
; body-conforming meshes; on hair/jewelry/accessories it is wasted work and
; drove a severe memory balloon (skee ApplyVertexDiff allocations retained by
; SSE Engine Fixes' allocator - HANDOVER §8).
function ToggleBodyMorph(int a_option, string contentId)
    bool nowOn = !CFW_Native.GetBodyMorph(contentId)
    if nowOn
        ShowMessage("CostumeFW: Body morph ON for '" + CFW_Native.GetItemName(contentId) + "'. Use ONLY for BodySlide body-conforming meshes (costumes/underwear). Hair, jewelry and accessories should stay OFF (wasted memory).", false)
    endIf
    CFW_Native.SetBodyMorph(contentId, nowOn)
    SetToggleOptionValue(a_option, nowOn)
endFunction

; -----------------------------------------------------------------------------
; Option handlers
; -----------------------------------------------------------------------------
event OnOptionSelect(int a_option)
    ; --- Main ---
    if a_option == _optEnable
        bool now = !CFW_Native.IsEnabled()
        CFW_Native.SetEnabled(now)
        SetToggleOptionValue(a_option, now)
        return
    elseIf a_option == _optReload
        CFW_Native.ReloadSettings()  ; full re-read of CEF_settings.json (queued)
        Debug.Notification("CostumeFW: reloading settings from disk...")
        ForcePageReset()
        return
    elseIf a_option == _optUninstall
        UninstallCleanup()
        return
    endIf

    ; --- Persist ---
    if a_option == _optRemoveAllPersist
        string[] pc = CFW_Native.GetPersistContents()
        int pi = 0
        while pi < pc.Length
            ; Return only what THIS save shows (P1-4): a non-active entry's
            ; original lives on the capturing character's save - fabricating a
            ; copy here would duplicate it for every character that bulk-removes.
            if CFW_Native.IsPersistActive(pc[pi])
                ReturnItem(CFW_Native.ResolveForm(pc[pi]), false)
            endIf
            CFW_Native.RemovePersist(pc[pi])
            pi += 1
        endWhile
        Debug.Notification("CostumeFW: removed all persist")
        ForcePageReset()
        return
    endIf
    int p = _persistRemoveOpts.Find(a_option)
    if p >= 0
        ReturnItem(CFW_Native.ResolveForm(_persistRemoveContents[p]))
        CFW_Native.RemovePersist(_persistRemoveContents[p])
        ForcePageReset()
        return
    endIf

    ; M2: [active on this save] toggle - VISUAL ONLY (items move only on
    ; capture / remove / uncataloged-deactivate, so toggle cycling can never
    ; duplicate an item).
    p = _persistActiveOpts.Find(a_option)
    if p >= 0
        string cid = _persistRemoveContents[p]
        bool nowActive = !CFW_Native.IsPersistActive(cid)
        if CFW_Native.SetPersistActive(cid, nowActive)
            SetToggleOptionValue(a_option, nowActive)
        else
            ShowMessage("CostumeFW: could not change the active state (see log).", false)
        endIf
        return
    endIf
    p = _persistMorphOpts.Find(a_option)
    if p >= 0
        ToggleBodyMorph(a_option, _persistRemoveContents[p])
        return
    endIf
    p = _persistUncatOpts.Find(a_option)
    if p >= 0
        ReturnItem(CFW_Native.ResolveForm(_persistUncatIds[p]))
        CFW_Native.SetPersistActive(_persistUncatIds[p], false)
        ForcePageReset()
        return
    endIf
    p = _morphOpts.Find(a_option)
    if p >= 0
        ToggleBodyMorph(a_option, _removeContents[p])
        return
    endIf

    ; --- Boxes (single-box page: box index is _curBoxIndex) ---
    int k = _distribOpts.Find(a_option)
    if k >= 0
        ToggleDistribute(a_option, _curBoxIndex)
        return
    endIf

    k = _equipOpts.Find(a_option)
    if k >= 0
        ToggleBoxToken(a_option, _equipBoxIdx[k])
        return
    endIf

    k = _deleteOpts.Find(a_option)
    if k >= 0
        string[] cont = CFW_Native.GetBoxContents(_curBoxIndex)
        int ci = 0
        while ci < cont.Length
            ReturnItem(CFW_Native.ResolveForm(cont[ci]), false)
            ci += 1
        endWhile
        Form tkn = CFW_Native.GetBoxTokenForm(_curBoxIndex)
        if tkn
            Game.GetPlayer().UnequipItem(tkn, false, true)
        endIf
        CFW_Native.RemoveBox(_deleteTokens[k])
        Debug.Notification("CostumeFW: box removed - reopen MCM to refresh the page list")
        ForcePageReset()
        return
    endIf

    k = _removeOpts.Find(a_option)
    if k >= 0
        ReturnItem(CFW_Native.ResolveForm(_removeContents[k]))
        CFW_Native.RemoveBoxContent(_removeTokens[k], _removeContents[k])
        ForcePageReset()
        return
    endIf
endEvent

; Human label for a FindContentHolder() result: the persist class, or the
; holding box's token name + slot.
string Function HolderLabel(string holder)
    if holder == "persist"
        return "Persist"
    endIf
    return CFW_Native.GetItemName(holder) + " (" + SlotName(CFW_Native.GetTokenSlot(holder)) + ")"
endFunction

ObjectReference Function GetStore()
    if _store == None
        Form base = Game.GetFormFromFile(0x00080D, "CostumeFW.esp")
        if base
            _store = Game.GetPlayer().PlaceAtMe(base)
            if _store
                _store.Disable()
            endIf
        endIf
    endIf
    return _store
endFunction

; Return ONE captured copy of akItem to the player: from this save's hidden
; store when it has one, else as a NEW copy. The new-copy fallback DUPLICATES
; across characters (the original stays in the capturing save's store) - that
; is ACCEPTED BY DESIGN: losing the item is worse, and an uncataloged stored
; copy cannot be returned from the MCM until the catalog/activate UI lands
; (in-game decision 2026-07-06, CEF_STATE_SCOPE.md §4). aNotify=false keeps
; bulk loops (remove-all / delete box / uninstall) quiet.
function ReturnItem(Form akItem, bool aNotify = true)
    if !akItem
        return
    endIf
    ObjectReference store = GetStore()
    if store && store.GetItemCount(akItem) > 0
        store.RemoveItem(akItem, 1, true, Game.GetPlayer())
    else
        Game.GetPlayer().AddItem(akItem, 1, true)
        if aNotify
            Debug.Notification("CostumeFW: returned as a new copy (none stored on this character)")
        endIf
    endIf
endFunction

; Clean-uninstall helper: return every captured item (box + persist) to the
; player, unequip + remove all box tokens, detach all injected meshes, and
; write enabled=false so a reload does NOT re-apply anything (review C: the
; old behavior kept enabled=true, so playing on re-applied everything). Defs
; stay in CEF_settings.json; re-enable from MCM Main to restore them.
function UninstallCleanup()
    Actor player = Game.GetPlayer()
    int n = CFW_Native.GetBoxCount()
    int i = 0
    while i < n
        string[] cont = CFW_Native.GetBoxContents(i)
        int ci = 0
        while ci < cont.Length
            ReturnItem(CFW_Native.ResolveForm(cont[ci]), false)
            ci += 1
        endWhile
        Form tkn = CFW_Native.GetBoxTokenForm(i)
        if tkn
            player.UnequipItem(tkn, false, true)
            player.RemoveItem(tkn, 99, true)
        endIf
        i += 1
    endWhile

    string[] pc = CFW_Native.GetPersistContents()
    int pi = 0
    while pi < pc.Length
        ; Active-set only (P1-4): same reasoning as Remove-all - do not fabricate
        ; copies of entries this character never activated.
        if CFW_Native.IsPersistActive(pc[pi])
            ReturnItem(CFW_Native.ResolveForm(pc[pi]), false)
        endIf
        pi += 1
    endWhile

    CFW_Native.Clear()  ; detach + unregister all injected meshes
    CFW_Native.SetEnabled(false)  ; persist the OFF state (no re-apply on reload)
    Debug.Notification("CostumeFW: items returned, tokens removed, CEF disabled - safe to uninstall.")
    ForcePageReset()
endFunction

function ToggleDistribute(int a_option, int a_boxIndex)
    string token = CFW_Native.GetBoxToken(a_boxIndex)
    Form tokenForm = CFW_Native.GetBoxTokenForm(a_boxIndex)
    bool wasOn = CFW_Native.GetBoxEnabled(a_boxIndex)
    Actor player = Game.GetPlayer()
    if wasOn
        CFW_Native.SetBoxEnabled(token, false)
        if tokenForm
            player.UnequipItem(tokenForm, false, true)
            player.RemoveItem(tokenForm, 99, true)
        endIf
    else
        CFW_Native.SetBoxEnabled(token, true)
        if tokenForm && player.GetItemCount(tokenForm) <= 0
            player.AddItem(tokenForm, 1, true)
        endIf
    endIf
    SetToggleOptionValue(a_option, !wasOn)
    ForcePageReset()
endFunction

function ToggleBoxToken(int a_option, int a_boxIndex)
    Form token = CFW_Native.GetBoxTokenForm(a_boxIndex)
    if !token
        Debug.Notification("CostumeFW: box token form not found")
        return
    endIf
    Actor player = Game.GetPlayer()
    bool worn = CFW_Native.IsBoxWorn(a_boxIndex)
    if worn
        player.UnequipItem(token, false, true)
    else
        if player.GetItemCount(token) <= 0
            player.AddItem(token, 1, true)
        endIf
        player.EquipItem(token, false, true)
    endIf
    SetToggleOptionValue(a_option, !worn)
endFunction

event OnOptionMenuOpen(int a_option)
    if a_option == _optNewBox
        _freeTokenIds = CFW_Native.GetFreeTokenIds()
        string[] slotOpts = Utility.CreateStringArray(_freeTokenIds.Length, "")
        int si = 0
        while si < _freeTokenIds.Length
            slotOpts[si] = CFW_Native.GetItemName(_freeTokenIds[si]) + ": " + SlotName(CFW_Native.GetTokenSlot(_freeTokenIds[si]))
            si += 1
        endWhile
        SetMenuDialogOptions(slotOpts)
        return
    endIf

    bool fromInv = (a_option == _optAddInvPersist || _addInvOpts.Find(a_option) >= 0)
    if fromInv || a_option == _optAddPersist || _addWornOpts.Find(a_option) >= 0
        ; One in-MCM SkyUI dialog that asks item AND gender NIF in a single pick:
        ; each item is listed three times (player / Male / Female). The inventory
        ; variant lists ALL carried armors (name-sorted, natively capped at 40) so
        ; an item can be captured without ever being equipped = no transient FSMP
        ; physics build.
        string[] names
        string[] ids
        if fromInv
            names = CFW_Native.GetInventoryItemNames()
            ids = CFW_Native.GetInventoryItemIds()
        else
            names = CFW_Native.GetWornItemNames()
            ids = CFW_Native.GetWornItemIds()
        endIf
        int n = names.Length
        _wornIdsCache    = Utility.CreateStringArray(n * 3, "")
        _wornGenderCache = Utility.CreateIntArray(n * 3, 0)
        string[] menu = Utility.CreateStringArray(n * 3, "")
        int i = 0
        while i < n
            menu[i * 3]     = names[i] + "  (player gender NIF)"
            menu[i * 3 + 1] = names[i] + "  (force Male NIF)"
            menu[i * 3 + 2] = names[i] + "  (force Female NIF)"
            _wornIdsCache[i * 3]     = ids[i]
            _wornIdsCache[i * 3 + 1] = ids[i]
            _wornIdsCache[i * 3 + 2] = ids[i]
            _wornGenderCache[i * 3]     = 0
            _wornGenderCache[i * 3 + 1] = 1
            _wornGenderCache[i * 3 + 2] = 2
            i += 1
        endWhile
        SetMenuDialogOptions(menu)
        return
    endIf

    int ai = _armorTypeOpts.Find(a_option)
    if ai >= 0
        string[] types = new string[3]
        types[0] = "Clothing"
        types[1] = "Light Armor"
        types[2] = "Heavy Armor"
        SetMenuDialogOptions(types)
        ; _curBoxIndex, not ai: ai is the option-array slot (always 0 - one
        ; armor-type row per single-box page), which pre-selected box 0's type.
        SetMenuDialogStartIndex(CFW_Native.GetBoxArmorType(_curBoxIndex))
        return
    endIf

    ; Box / persist "Preset" menu: "(manual)" + preset names; files cached parallel ([0]="").
    if _assignOpts.Find(a_option) >= 0 || a_option == _persistPresetOpt
        string[] names = CFW_Native.GetPresetNames()
        _presetFilesCache = Utility.CreateStringArray(names.Length + 1, "")
        string[] files = CFW_Native.GetPresetFiles()
        string[] menuOpts = Utility.CreateStringArray(names.Length + 1, "")
        menuOpts[0] = "(manual)"
        int i = 0
        while i < names.Length
            menuOpts[i + 1] = names[i]
            _presetFilesCache[i + 1] = files[i]
            i += 1
        endWhile
        SetMenuDialogOptions(menuOpts)
        return
    endIf

    ; Presets page "Assign to box" menu: list every box.
    if _presetAssignOpts.Find(a_option) >= 0
        int n = CFW_Native.GetBoxCount()
        _presetBoxTokens = Utility.CreateStringArray(n + 1, "")
        string[] boxOpts = Utility.CreateStringArray(n + 1, "")
        boxOpts[0] = "(cancel)"
        int i = 0
        while i < n
            string tk = CFW_Native.GetBoxToken(i)
            boxOpts[i + 1] = CFW_Native.GetItemName(tk) + ": " + SlotName(CFW_Native.GetTokenSlot(tk))
            _presetBoxTokens[i + 1] = tk
            i += 1
        endWhile
        SetMenuDialogOptions(boxOpts)
    endIf
endEvent

event OnOptionMenuAccept(int a_option, int a_index)
    ; --- "+ New box" slot picker ---
    if a_option == _optNewBox
        if a_index >= 0 && a_index < _freeTokenIds.Length
            string newToken = _freeTokenIds[a_index]
            CFW_Native.AddBox(CFW_Native.GetItemName(newToken), newToken, "")
            BuildPages()  ; include the new box's page (visible after reopening the MCM)
            Debug.Notification("CostumeFW: box created for " + SlotName(CFW_Native.GetTokenSlot(newToken)) + " - reopen MCM to open its page")
            ForcePageReset()
        endIf
        return
    endIf

    ; --- Persist "+ Add worn item" / "+ Add from inventory" capture ---
    if a_option == _optAddPersist || a_option == _optAddInvPersist
        if a_index < 0 || a_index >= _wornIdsCache.Length
            return
        endIf
        string contentId = _wornIdsCache[a_index]
        ; Cross-holder guard (P1-1): the injection registry is one-entry-per-id,
        ; so an id already in a BOX must not also enter persist. A persist->
        ; persist duplicate stays AddPersist's own same-save check below.
        string holder = CFW_Native.FindContentHolder(contentId)
        if holder != "" && holder != "persist"
            ShowMessage("CostumeFW: already captured in " + HolderLabel(holder) + " - item not moved.", false)
            ForcePageReset()
            return
        endIf
        ; Transaction order (review A-2): register FIRST, move the item only on
        ; success. A duplicate add must not swallow a second physical copy.
        if !CFW_Native.AddPersist(contentId)
            ; Modal: a Debug.Notification is easy to miss while the MCM stays
            ; open (in-game feedback 2026-07-06).
            ShowMessage("CostumeFW: already in persist - item not moved.", false)
            ForcePageReset()
            return
        endIf
        ; Per-content settings only AFTER the add succeeded (P1-3): a failed
        ; duplicate capture must not overwrite the existing entry's gender /
        ; enchant snapshot. Still before the move - the item is owned/worn here.
        CFW_Native.SetContentGender(contentId, _wornGenderCache[a_index])  ; picked in the menu
        CFW_Native.CaptureContentEnchant(contentId)             ; snapshot enchant while worn
        Form contentForm = CFW_Native.ResolveForm(contentId)
        if contentForm
            Game.GetPlayer().RemoveItem(contentForm, 1, true, GetStore())
        endIf
        Debug.Notification("CostumeFW: persist added " + CFW_Native.GetItemName(contentId))
        WarnIfScripted(contentId)
        ForcePageReset()
        return
    endIf

    ; --- Box "+ Add worn item" / "+ Add from inventory" capture ---
    int k = _addWornOpts.Find(a_option)
    int capBoxIdx = -1
    if k >= 0
        capBoxIdx = _addWornBoxIdx[k]
    else
        k = _addInvOpts.Find(a_option)
        if k >= 0
            capBoxIdx = _addInvBoxIdx[k]
        endIf
    endIf
    if capBoxIdx >= 0
        if a_index < 0 || a_index >= _wornIdsCache.Length
            return
        endIf
        int boxIdx = capBoxIdx
        string token = CFW_Native.GetBoxToken(boxIdx)
        string contentId = _wornIdsCache[a_index]

        ; Cross-holder guard (P1-1): block capture into a SECOND box, or when
        ; persist holds the id. Same-box duplicates stay AddBox's own check.
        string holder = CFW_Native.FindContentHolder(contentId)
        if holder != "" && holder != token
            ShowMessage("CostumeFW: already captured in " + HolderLabel(holder) + " - item not moved.", false)
            ForcePageReset()
            return
        endIf
        ; Transaction order (review A-2): register FIRST, move the item only on
        ; success (false = duplicate/bad input; do not swallow another copy).
        if !CFW_Native.AddBox("", token, contentId)
            ; Modal: a Debug.Notification is easy to miss while the MCM stays
            ; open (in-game feedback 2026-07-06).
            ShowMessage("CostumeFW: already in this box - item not moved.", false)
            ForcePageReset()
            return
        endIf
        ; Per-content settings only AFTER the add succeeded (P1-3) - see the
        ; persist flow above.
        CFW_Native.SetContentGender(contentId, _wornGenderCache[a_index])  ; picked in the menu
        CFW_Native.CaptureContentEnchant(contentId)             ; snapshot enchant while worn
        Form contentForm = CFW_Native.ResolveForm(contentId)
        Actor player = Game.GetPlayer()
        if contentForm
            player.RemoveItem(contentForm, 1, true, GetStore())
        endIf
        Form tokenForm = CFW_Native.GetBoxTokenForm(boxIdx)
        if tokenForm && !CFW_Native.IsBoxWorn(boxIdx)
            if player.GetItemCount(tokenForm) <= 0
                player.AddItem(tokenForm, 1, true)
            endIf
            player.EquipItem(tokenForm, false, true)
        endIf
        Debug.Notification("CostumeFW: captured " + CFW_Native.GetItemName(contentId))
        WarnIfScripted(contentId)
        ForcePageReset()
        return
    endIf

    ; --- "Armor type" menu ---
    k = _armorTypeOpts.Find(a_option)
    if k >= 0
        if a_index >= 0 && a_index <= 2
            CFW_Native.SetBoxArmorType(_armorTypeTokens[k], a_index)
            Form tf = CFW_Native.GetBoxTokenForm(_curBoxIndex)
            if tf && CFW_Native.IsBoxWorn(_curBoxIndex)
                Actor pl = Game.GetPlayer()
                pl.UnequipItem(tf, false, true)
                pl.EquipItem(tf, false, true)
            endIf
            ForcePageReset()
        endIf
        return
    endIf

    ; --- Persist "Preset" assign menu (0 = manual/clear) ---
    if a_option == _persistPresetOpt
        if a_index <= 0
            CFW_Native.ClearPersistPreset()
        else
            if !CFW_Native.AssignPersistPreset(_presetFilesCache[a_index])
                Debug.Notification("CostumeFW: preset already assigned to a box")
            endIf
        endIf
        ForcePageReset()
        return
    endIf

    ; --- Box "Preset" assign menu (0 = manual/clear) ---
    k = _assignOpts.Find(a_option)
    if k >= 0
        string token = _assignTokens[k]
        if a_index <= 0
            CFW_Native.ClearPreset(token)
        else
            if !CFW_Native.AssignPreset(token, _presetFilesCache[a_index])
                Debug.Notification("CostumeFW: preset already assigned to another box")
            endIf
        endIf
        ForcePageReset()
        return
    endIf

    ; --- Presets page "Assign to box" menu (0 = cancel) ---
    k = _presetAssignOpts.Find(a_option)
    if k >= 0
        if a_index > 0 && a_index < _presetBoxTokens.Length
            if !CFW_Native.AssignPreset(_presetBoxTokens[a_index], _presetAssignFiles[k])
                Debug.Notification("CostumeFW: assign failed (already used or box invalid)")
            else
                Debug.Notification("CostumeFW: preset assigned")
            endIf
            ForcePageReset()
        endIf
        return
    endIf
endEvent

event OnOptionInputOpen(int a_option)
    int k = _exportOpts.Find(a_option)
    if k >= 0
        string cur = CFW_Native.GetBoxPreset(_exportTokens[k])
        if cur == ""
            cur = CFW_Native.GetItemName(_exportTokens[k])
        endIf
        SetInputDialogStartText(cur)
        return
    endIf

    k = _hideOpts.Find(a_option)
    if k >= 0
        SetInputDialogStartText(CFW_Native.GetHideSlots(_removeContents[k]))
        return
    endIf

    k = _persistHideOpts.Find(a_option)
    if k >= 0
        SetInputDialogStartText(CFW_Native.GetHideSlots(_persistRemoveContents[k]))
        return
    endIf

    if a_option == _persistExportOpt
        string cur = CFW_Native.GetPersistPreset()
        if cur == ""
            cur = "Persist"
        endIf
        SetInputDialogStartText(cur)
    endIf
endEvent

event OnOptionInputAccept(int a_option, string a_input)
    int k = _exportOpts.Find(a_option)
    if k >= 0
        if a_input != ""
            string file = CFW_Native.ExportPreset(_exportTokens[k], a_input)
            if file != ""
                Debug.Notification("CostumeFW: exported preset " + file)
            endIf
        endIf
        ForcePageReset()
        return
    endIf

    k = _hideOpts.Find(a_option)
    if k >= 0
        CFW_Native.SetHideSlots(_removeContents[k], a_input)
        ForcePageReset()
        return
    endIf

    k = _persistHideOpts.Find(a_option)
    if k >= 0
        CFW_Native.SetHideSlots(_persistRemoveContents[k], a_input)
        ForcePageReset()
        return
    endIf

    if a_option == _persistExportOpt
        if a_input != ""
            string file = CFW_Native.ExportPersist(a_input)
            if file != ""
                Debug.Notification("CostumeFW: exported persist preset " + file)
            endIf
        endIf
        ForcePageReset()
    endIf
endEvent

event OnOptionHighlight(int a_option)
    if a_option == _optEnable
        SetInfoText("Master on/off for the whole framework. Off hides everything.")
    elseIf a_option == _optReload
        SetInfoText("Re-apply settings. A full re-read of CEF_settings.json happens on game load.")
    elseIf a_option == _optUninstall
        SetInfoText("Return all captured items, remove all box tokens, detach everything. Do this before removing the mod.")
    elseIf a_option == _optAddPersist
        SetInfoText("Capture a worn accessory as an always-on persist item (frees its slot). Asks which gender NIF to load.")
    elseIf a_option == _persistPresetOpt
        SetInfoText("Assign a preset's contents to the persist set (same presets as boxes; one preset per box/persist).")
    elseIf a_option == _persistExportOpt
        SetInfoText("Save the current persist items as a shareable CEFP_*.json preset.")
    elseIf a_option == _optNewBox
        SetInfoText("Create a new box on a free Costume Box slot.")
    elseIf _equipOpts.Find(a_option) >= 0
        int bi = _equipBoxIdx[_equipOpts.Find(a_option)]
        SetInfoText("Wear/remove the box token (worn = contents show). Contains: " + BoxContentsSummary(bi))
    elseIf _addWornOpts.Find(a_option) >= 0
        SetInfoText("Capture a currently-worn armor into this box (frees its slot). Asks which gender NIF to load.")
    elseIf _assignOpts.Find(a_option) >= 0
        SetInfoText("Assign a preset's contents to this box (one preset per box).")
    elseIf _exportOpts.Find(a_option) >= 0
        SetInfoText("Save this box's contents as a shareable CEFP_*.json preset.")
    elseIf _deleteOpts.Find(a_option) >= 0
        SetInfoText("Delete this box (its token becomes free; items returned).")
    elseIf _removeOpts.Find(a_option) >= 0
        SetInfoText("Remove this item from the box (returned to you). Box contents are shared across saves.")
    elseIf _persistRemoveOpts.Find(a_option) >= 0
        SetInfoText("Remove from the SHARED catalog (all saves). Deactivates here and returns the item; other characters keep it active until they deactivate.")
    elseIf _persistActiveOpts.Find(a_option) >= 0
        SetInfoText("Show this catalog item on THIS save. The catalog is shared; each save picks what it shows. No items are moved by this toggle.")
    elseIf _persistMorphOpts.Find(a_option) >= 0 || _morphOpts.Find(a_option) >= 0
        SetInfoText("Apply your RaceMenu body sliders to this mesh. ON only for BodySlide body-conforming meshes; keep OFF for hair/jewelry (memory cost).")
    elseIf _persistUncatOpts.Find(a_option) >= 0
        SetInfoText("Active on this save but removed from the shared catalog elsewhere. Deactivate to stop showing it and get the item back.")
    elseIf a_option == _optAddInvPersist || _addInvOpts.Find(a_option) >= 0
        SetInfoText("Capture straight from your inventory - no need to equip first (skips the transient physics build). Player-enchanted items keep their enchantment. Lists the first 40 armors by name; for others, equip and use + Add worn item.")
    elseIf _hideOpts.Find(a_option) >= 0 || _persistHideOpts.Find(a_option) >= 0
        SetInfoText("Hide this item while a real item holds a slot. Enter slot numbers (e.g. 37 = feet/boots, or 30 31 42 = helmet/hair/circlet). Blank = never hide.")
    elseIf _presetAssignOpts.Find(a_option) >= 0
        SetInfoText("Assign this preset to one of your boxes.")
    endIf
endEvent

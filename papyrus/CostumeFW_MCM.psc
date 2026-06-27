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

int property CURRENT_VERSION = 10 autoReadonly

; --- Main page state ---
int _optEnable
int _optReload
int _optUninstall

; --- Persist page state ---
int      _optAddPersist
int      _optRemoveAllPersist
int[]    _persistRemoveOpts
string[] _persistRemoveContents
string[] _persistWornIdsCache

; --- Boxes page state ---
int _optNewBox
int[]    _equipOpts
int[]    _equipBoxIdx
int[]    _addWornOpts
int[]    _addWornBoxIdx
int[]    _deleteOpts
string[] _deleteTokens
int[]    _removeOpts
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
    Pages = new string[4]
    Pages[0] = "Main"
    Pages[1] = "Persist"
    Pages[2] = "Boxes"
    Pages[3] = "Presets"

    ; Init every option-map array so a Find() before a page is built is safe.
    _persistRemoveOpts     = new int[1]
    _persistRemoveContents = new string[1]
    _persistWornIdsCache   = new string[1]
    _equipOpts     = new int[1]
    _equipBoxIdx   = new int[1]
    _addWornOpts   = new int[1]
    _addWornBoxIdx = new int[1]
    _deleteOpts    = new int[1]
    _deleteTokens  = new string[1]
    _removeOpts     = new int[1]
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
    _freeTokenIds    = new string[1]
    _presetFilesCache = new string[1]
    _presetAssignOpts = new int[1]
    _presetAssignFiles = new string[1]
    _presetBoxTokens = new string[1]
endFunction

int function GetVersion()
    return CURRENT_VERSION
endFunction

event OnPageReset(string a_page)
    if a_page == "Boxes"
        ResetBoxesPage()
    elseIf a_page == "Persist"
        ResetPersistPage()
    elseIf a_page == "Presets"
        ResetPresetsPage()
    else
        ResetMainPage()
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
    if a_slot == 44
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
    if Game.GetFormFromFile(0x00080D, "CostumeFW_Boxes.esp")
        boxesEsp = "OK"
    endIf
    AddTextOption("CostumeFW_Boxes.esp", boxesEsp, OPTION_FLAG_DISABLED)

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

    string[] contents = CFW_Native.GetPersistContents()
    AddHeaderOption("Persist (" + contents.Length + ")")
    if contents.Length == 0
        AddTextOption("(none - use + Add worn item)", "", OPTION_FLAG_DISABLED)
        _persistRemoveOpts     = new int[1]
        _persistRemoveContents = new string[1]
    else
        _persistRemoveOpts     = Utility.CreateIntArray(contents.Length, -1)
        _persistRemoveContents = Utility.CreateStringArray(contents.Length, "")
        int i = 0
        while i < contents.Length
            _persistRemoveOpts[i] = AddTextOption(CFW_Native.GetItemName(contents[i]), "[remove]")
            _persistRemoveContents[i] = contents[i]
            i += 1
        endWhile
        AddHeaderOption("Actions")
        _optRemoveAllPersist = AddTextOption("Remove all persist", "")
    endIf
endFunction

; -----------------------------------------------------------------------------
; Boxes page
; -----------------------------------------------------------------------------
function ResetBoxesPage()
    SetCursorFillMode(TOP_TO_BOTTOM)

    int n = CFW_Native.GetBoxCount()
    _optNewBox = AddMenuOption("+ New box (pick slot)", "")
    AddHeaderOption("Boxes (" + n + ")")

    if n == 0
        AddTextOption("(none yet - use + New box)", "", OPTION_FLAG_DISABLED)
        _equipOpts     = new int[1]
        _equipBoxIdx   = new int[1]
        _addWornOpts   = new int[1]
        _addWornBoxIdx = new int[1]
        _distribOpts   = new int[1]
        _distribTokens = new string[1]
        _armorTypeOpts   = new int[1]
        _armorTypeTokens = new string[1]
        _assignOpts    = new int[1]
        _assignTokens  = new string[1]
        _exportOpts    = new int[1]
        _exportTokens  = new string[1]
        _deleteOpts    = new int[1]
        _deleteTokens  = new string[1]
        _removeOpts     = new int[1]
        _removeTokens   = new string[1]
        _removeContents = new string[1]
        return
    endIf

    int total = 0
    int i = 0
    while i < n
        total += CFW_Native.GetBoxContents(i).Length
        i += 1
    endWhile
    if total == 0
        total = 1
    endIf

    _equipOpts     = Utility.CreateIntArray(n, -1)
    _equipBoxIdx   = Utility.CreateIntArray(n, -1)
    _addWornOpts   = Utility.CreateIntArray(n, -1)
    _addWornBoxIdx = Utility.CreateIntArray(n, -1)
    _distribOpts   = Utility.CreateIntArray(n, -1)
    _distribTokens = Utility.CreateStringArray(n, "")
    _armorTypeOpts   = Utility.CreateIntArray(n, -1)
    _armorTypeTokens = Utility.CreateStringArray(n, "")
    _assignOpts    = Utility.CreateIntArray(n, -1)
    _assignTokens  = Utility.CreateStringArray(n, "")
    _exportOpts    = Utility.CreateIntArray(n, -1)
    _exportTokens  = Utility.CreateStringArray(n, "")
    _deleteOpts    = Utility.CreateIntArray(n, -1)
    _deleteTokens  = Utility.CreateStringArray(n, "")
    _removeOpts     = Utility.CreateIntArray(total, -1)
    _removeTokens   = Utility.CreateStringArray(total, "")
    _removeContents = Utility.CreateStringArray(total, "")

    int ri = 0
    i = 0
    while i < n
        string token = CFW_Native.GetBoxToken(i)
        int slot = CFW_Native.GetTokenSlot(token)
        AddHeaderOption(CFW_Native.GetItemName(token) + ": " + SlotName(slot))
        _distribOpts[i] = AddToggleOption("Distribute token", CFW_Native.GetBoxEnabled(i))
        _distribTokens[i] = token
        _equipOpts[i] = AddToggleOption("Wear (show contents)", CFW_Native.IsBoxWorn(i))
        _equipBoxIdx[i] = i
        _addWornOpts[i] = AddMenuOption("+ Add worn item", "")
        _addWornBoxIdx[i] = i
        _armorTypeOpts[i] = AddMenuOption("Armor type", ArmorTypeName(CFW_Native.GetBoxArmorType(i)))
        _armorTypeTokens[i] = token
        string presetName = CFW_Native.GetBoxPreset(token)
        if presetName == ""
            presetName = "(manual)"
        endIf
        _assignOpts[i] = AddMenuOption("Preset", presetName)
        _assignTokens[i] = token
        _exportOpts[i] = AddInputOption("Export as preset", "")
        _exportTokens[i] = token
        AddTextOption("Stats", CFW_Native.GetBoxStats(i), OPTION_FLAG_DISABLED)
        _deleteOpts[i] = AddTextOption("Delete box", "")
        _deleteTokens[i] = token

        string[] contents = CFW_Native.GetBoxContents(i)
        int ci = 0
        while ci < contents.Length
            _removeOpts[ri] = AddTextOption("  " + CFW_Native.GetItemName(contents[ci]), "[remove]")
            _removeTokens[ri] = token
            _removeContents[ri] = contents[ci]
            ri += 1
            ci += 1
        endWhile
        i += 1
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
        CFW_Native.SetEnabled(CFW_Native.IsEnabled())  ; no-op write; reconcile re-reads scene
        Debug.Notification("CostumeFW: settings reload requested (restart for full re-read)")
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
            ReturnItem(CFW_Native.ResolveForm(pc[pi]))
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

    ; --- Boxes ---
    int k = _distribOpts.Find(a_option)
    if k >= 0
        ToggleDistribute(a_option, k)
        return
    endIf

    k = _equipOpts.Find(a_option)
    if k >= 0
        ToggleBoxToken(a_option, _equipBoxIdx[k])
        return
    endIf

    k = _deleteOpts.Find(a_option)
    if k >= 0
        string[] cont = CFW_Native.GetBoxContents(k)
        int ci = 0
        while ci < cont.Length
            ReturnItem(CFW_Native.ResolveForm(cont[ci]))
            ci += 1
        endWhile
        Form tkn = CFW_Native.GetBoxTokenForm(k)
        if tkn
            Game.GetPlayer().UnequipItem(tkn, false, true)
        endIf
        CFW_Native.RemoveBox(_deleteTokens[k])
        Debug.Notification("CostumeFW: box removed (items returned)")
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

ObjectReference Function GetStore()
    if _store == None
        Form base = Game.GetFormFromFile(0x00080D, "CostumeFW_Boxes.esp")
        if base
            _store = Game.GetPlayer().PlaceAtMe(base)
            if _store
                _store.Disable()
            endIf
        endIf
    endIf
    return _store
endFunction

function ReturnItem(Form akItem)
    if !akItem
        return
    endIf
    ObjectReference store = GetStore()
    if store && store.GetItemCount(akItem) > 0
        store.RemoveItem(akItem, 1, true, Game.GetPlayer())
    else
        Game.GetPlayer().AddItem(akItem, 1, true)
    endIf
endFunction

; Clean-uninstall helper: return every captured item (box + persist) to the
; player, unequip + remove all box tokens, and detach all injected meshes. After
; this it is safe to remove the mod. (Defs in CEF_settings.json are left as-is;
; if you keep playing, a reload re-applies them.)
function UninstallCleanup()
    Actor player = Game.GetPlayer()
    int n = CFW_Native.GetBoxCount()
    int i = 0
    while i < n
        string[] cont = CFW_Native.GetBoxContents(i)
        int ci = 0
        while ci < cont.Length
            ReturnItem(CFW_Native.ResolveForm(cont[ci]))
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
        ReturnItem(CFW_Native.ResolveForm(pc[pi]))
        pi += 1
    endWhile

    CFW_Native.Clear()  ; detach + unregister all injected meshes
    Debug.Notification("CostumeFW: items returned, tokens removed - safe to uninstall.")
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

    if a_option == _optAddPersist || _addWornOpts.Find(a_option) >= 0
        _wornIdsCache = CFW_Native.GetWornItemIds()
        SetMenuDialogOptions(CFW_Native.GetWornItemNames())
        return
    endIf

    int ai = _armorTypeOpts.Find(a_option)
    if ai >= 0
        string[] types = new string[3]
        types[0] = "Clothing"
        types[1] = "Light Armor"
        types[2] = "Heavy Armor"
        SetMenuDialogOptions(types)
        SetMenuDialogStartIndex(CFW_Native.GetBoxArmorType(ai))
        return
    endIf

    ; Box "Preset" menu: "(manual)" + preset names; files cached parallel ([0]="").
    if _assignOpts.Find(a_option) >= 0
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
            Debug.Notification("CostumeFW: box created for " + SlotName(CFW_Native.GetTokenSlot(newToken)))
            ForcePageReset()
        endIf
        return
    endIf

    ; --- Persist "+ Add worn item" capture ---
    if a_option == _optAddPersist
        if a_index < 0 || a_index >= _wornIdsCache.Length
            return
        endIf
        string contentId = _wornIdsCache[a_index]
        Form contentForm = CFW_Native.ResolveForm(contentId)
        if contentForm
            Game.GetPlayer().RemoveItem(contentForm, 1, true, GetStore())
        endIf
        CFW_Native.AddPersist(contentId)
        Debug.Notification("CostumeFW: persist added " + CFW_Native.GetItemName(contentId))
        ForcePageReset()
        return
    endIf

    ; --- Box "+ Add worn item" capture ---
    int k = _addWornOpts.Find(a_option)
    if k >= 0
        if a_index < 0 || a_index >= _wornIdsCache.Length
            return
        endIf
        int boxIdx = _addWornBoxIdx[k]
        string token = CFW_Native.GetBoxToken(boxIdx)
        string contentId = _wornIdsCache[a_index]

        CFW_Native.AddBox("", token, contentId)
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
        ForcePageReset()
        return
    endIf

    ; --- "Armor type" menu ---
    k = _armorTypeOpts.Find(a_option)
    if k >= 0
        if a_index >= 0 && a_index <= 2
            CFW_Native.SetBoxArmorType(_armorTypeTokens[k], a_index)
            Form tf = CFW_Native.GetBoxTokenForm(k)
            if tf && CFW_Native.IsBoxWorn(k)
                Actor pl = Game.GetPlayer()
                pl.UnequipItem(tf, false, true)
                pl.EquipItem(tf, false, true)
            endIf
            ForcePageReset()
        endIf
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
        SetInfoText("Capture a worn accessory as an always-on persist item (frees its slot).")
    elseIf a_option == _optNewBox
        SetInfoText("Create a new box on a free Costume Box slot.")
    elseIf _equipOpts.Find(a_option) >= 0
        SetInfoText("Wear/remove the box token. Worn = its captured contents show.")
    elseIf _addWornOpts.Find(a_option) >= 0
        SetInfoText("Capture a currently-worn armor into this box (frees its slot).")
    elseIf _assignOpts.Find(a_option) >= 0
        SetInfoText("Assign a preset's contents to this box (one preset per box).")
    elseIf _exportOpts.Find(a_option) >= 0
        SetInfoText("Save this box's contents as a shareable CEFP_*.json preset.")
    elseIf _deleteOpts.Find(a_option) >= 0
        SetInfoText("Delete this box (its token becomes free; items returned).")
    elseIf _removeOpts.Find(a_option) >= 0
        SetInfoText("Remove this item from the box (returned to you).")
    elseIf _persistRemoveOpts.Find(a_option) >= 0
        SetInfoText("Remove this persist item (returned to you).")
    elseIf _presetAssignOpts.Find(a_option) >= 0
        SetInfoText("Assign this preset to one of your boxes.")
    endIf
endEvent

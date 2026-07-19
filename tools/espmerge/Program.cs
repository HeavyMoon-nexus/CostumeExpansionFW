// espmerge - one-shot v1.2.1 plugin consolidation for Costume Expansion FW.
// CostumeFW_NPC.esp is a PERMANENT separate add-on - NEVER fold it.
//
// Folds CostumeFW_Boxes_FSMPCarrier_001.esp (houseCARL patch: overrides +
// new records) into CostumeFW_Boxes.esp and writes a single ESL-flagged
// CostumeFW.esp (espfe):
//   - base-defined records keep their local FormIDs (patch overrides folded
//     in as the record content = the load-order winner)
//   - patch-NEW records are renumbered +0x100 (0x8xx -> 0x9xx) because the
//     two plugins' local id ranges collide record-for-record
//   - the three PoC head-carrier leftovers (patch 0x806-0x808) are DROPPED
//     (approach-C PoC garbage; purge from saves BEFORE switching plugins)
//   - all internal links are remapped; editorIDs are preserved (the physics
//     XML / nifcarrier side addresses head parts by editorID)
//   - a SEQ file is emitted for the StartGameEnabled MCM quest (0x802) so
//     the quest starts when the renamed plugin joins an existing save.
//
// The +0x100 rule is load-bearing: the CEF runtime (BoxStore.cpp) and the
// settings-JSON migration use the same mapping. Keep them in sync.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

internal static class Program
{
    private const string kDefaultBase =
        @"K:\Mo2_SkyrimSE1170\mods\houseCARL - CostumeFW_Boxes\CostumeFW_Boxes.esp";
    private const string kDefaultPatch =
        @"K:\Mo2_SkyrimSE1170\mods\houseCARL - CostumeFW_Boxes_FSMPCarrier_001\CostumeFW_Boxes_FSMPCarrier_001.esp";
    private const string kDefaultOutDir =
        @"K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW";

    private const string kOutName = "CostumeFW.esp";
    // CONTRACT (border audit ROOT J): the runtime id-healer MigrateLegacyColonId
    // (src/BoxStore.cpp) MUST mirror this disposition - it re-maps a pre-merge
    // settings.json's colon-ids by the same +kRenumberOffset for patch-new records
    // and drops kDropPatchIds (leaving them unhealed). Change both sides together.
    private const uint kRenumberOffset = 0x100;   // patch-new 0x8xx -> 0x9xx
    private const uint kMcmQuestLocalId = 0x802;  // CFW_MCMQuest (StartGameEnabled)

    // Patch-local ids NOT carried over (PoC head-carrier leftovers).

    private static readonly uint[] kDropPatchIds = { 0x806, 0x807, 0x808 };

    private static int Main(string[] args)
    {
        if (args.Length >= 2 && args[0] == "--verify-npc") {
            try { return VerifyNpcAddon(args[1]); }
            catch (Exception ex) { Console.Error.WriteLine("FATAL: " + ex); return 1; }
        }
        if (args.Length >= 3 && args[0] == "--build-npc") {
            try {
                var result = BuildNpcAddon(args[1], args[2]);
                if (result != 0) return result;
                return VerifyNpcAddon(Path.Combine(args[2], "CostumeFW_NPC.esp"));
            } catch (Exception ex) {
                Console.Error.WriteLine("FATAL: " + ex);
                return 1;
            }
        }

        var basePath = args.Length > 0 ? args[0] : kDefaultBase;
        var patchPath = args.Length > 1 ? args[1] : kDefaultPatch;
        var outDir = args.Length > 2 ? args[2] : kDefaultOutDir;
        // Optional 4th arg: patch-new renumber offset in hex (default 0x100).
        // v1.3.0 VanillaSlots fold uses 0x200 (0x9xx is taken by the v1.2.1
        // renumber). CONTRACT: MigrateLegacyColonId mirrors this per patch.
        var offset = args.Length > 3 ? Convert.ToUInt32(args[3], 16) : kRenumberOffset;

        try {
            return Run(basePath, patchPath, outDir, offset);
        } catch (Exception ex) {
            Console.Error.WriteLine("FATAL: " + ex);
            return 1;
        }
    }

    private static readonly uint[] kNpcRaceIds = {
        0x013740, 0x013741, 0x013742, 0x013743, 0x013744,
        0x013745, 0x013746, 0x013747, 0x013748, 0x013749,
        0x08883A, 0x08883C, 0x08883D, 0x088840, 0x088844,
        0x088845, 0x088794, 0x0A82B9, 0x088846, 0x088884
    };

    private static int BuildNpcAddon(string corePath, string outDir)
    {
        if (!File.Exists(corePath)) {
            Console.Error.WriteLine("missing core template: " + corePath);
            return 1;
        }
        var source = SkyrimMod.CreateFromBinaryOverlay(
            ModPath.FromPath(corePath), SkyrimRelease.SkyrimSE);
        var armorTemplate = source.Armors.FirstOrDefault(x => x.FormKey.ID == 0x801);
        var addonTemplate = source.ArmorAddons.FirstOrDefault(x => x.FormKey.ID == 0x900);
        if (armorTemplate == null || addonTemplate == null) {
            Console.Error.WriteLine("core template records 000801/000900 are missing");
            return 1;
        }

        var outKey = ModKey.FromNameAndExtension("CostumeFW_NPC.esp");
        var skyrimKey = ModKey.FromNameAndExtension("Skyrim.esm");
        var outMod = new SkyrimMod(outKey, SkyrimRelease.SkyrimSE);
        outMod.ModHeader.Flags |= (SkyrimModHeader.HeaderFlag)0x200;

        for (var i = 0; i < 8; ++i) {
            var armorKey = new FormKey(outKey, (uint)(0x800 + i));
            var addonKey = new FormKey(outKey, (uint)(0x808 + i));
            var armor = armorTemplate.Duplicate(armorKey);
            var addon = addonTemplate.Duplicate(addonKey);
            armor.EditorID = $"CFW_PubToken{i + 1:00}";
            armor.Name = "Costume (unpublished)";
            armor.MajorFlags &= ~Armor.MajorFlag.NonPlayable;
            armor.BodyTemplate.FirstPersonFlags = (BipedObjectFlag)(1u << (44 - 30));
            armor.Armature.Clear();
            armor.Armature.Add(new FormLink<IArmorAddonGetter>(addonKey));
            addon.EditorID = $"CFW_PubCarrier{i + 1:00}";
            addon.BodyTemplate.FirstPersonFlags = (BipedObjectFlag)(1u << (44 - 30));
            // No "Meshes\" prefix: ARMA world-model paths are Data\meshes-relative
            // (core CostumeFW.esp box carriers and RepointCarrierSexed both write
            // "CostumeFW\..."; BSModelDB::Demand rejects a leading meshes\).
            addon.WorldModel.Male.File = $@"CostumeFW\Pub{i + 1:00}_carrier_m_r0.nif";
            addon.WorldModel.Female.File = $@"CostumeFW\Pub{i + 1:00}_carrier_f_r0.nif";
            SetNpcRaces(addon, skyrimKey);
            outMod.Armors.RecordCache.Set(armor);
            outMod.ArmorAddons.RecordCache.Set(addon);
        }

        for (var i = 0; i < 8; ++i) {
            var armorKey = new FormKey(outKey, (uint)(0x810 + i));
            var addonKey = new FormKey(outKey, (uint)(0x818 + i));
            var armor = armorTemplate.Duplicate(armorKey);
            var addon = addonTemplate.Duplicate(addonKey);
            armor.EditorID = $"CFW_NpcPersistTok{i + 1:00}";
            armor.Name = "Costume NPC carrier";
            armor.MajorFlags |= Armor.MajorFlag.NonPlayable;
            armor.BodyTemplate.FirstPersonFlags = (BipedObjectFlag)(1u << (54 - 30));
            armor.Armature.Clear();
            armor.Armature.Add(new FormLink<IArmorAddonGetter>(addonKey));
            addon.EditorID = $"CFW_NpcPersistCar{i + 1:00}";
            addon.BodyTemplate.FirstPersonFlags = (BipedObjectFlag)(1u << (54 - 30));
            var file = $@"CostumeFW\NpcPersist{i + 1:00}_carrier_r0.nif";
            addon.WorldModel.Male.File = file;
            addon.WorldModel.Female.File = file;
            SetNpcRaces(addon, skyrimKey);
            outMod.Armors.RecordCache.Set(armor);
            outMod.ArmorAddons.RecordCache.Set(addon);
        }

        Directory.CreateDirectory(outDir);
        var outPath = Path.Combine(outDir, "CostumeFW_NPC.esp");
        outMod.WriteToBinary(outPath);
        Console.WriteLine("npc add-on -> " + outPath);
        return 0;
    }

    private static void SetNpcRaces(ArmorAddon addon, ModKey skyrimKey)
    {
        addon.Race.SetTo(new FormKey(skyrimKey, 0x000019));
        addon.AdditionalRaces.Clear();
        foreach (var id in kNpcRaceIds)
            addon.AdditionalRaces.Add(new FormLink<IRaceGetter>(new FormKey(skyrimKey, id)));
    }

    private static int VerifyNpcAddon(string path)
    {
        if (!File.Exists(path)) {
            Console.Error.WriteLine("missing NPC add-on: " + path);
            return 1;
        }
        var mod = SkyrimMod.CreateFromBinaryOverlay(
            ModPath.FromPath(path), SkyrimRelease.SkyrimSE);
        var errors = new List<string>();
        if ((mod.ModHeader.Flags & (SkyrimModHeader.HeaderFlag)0x200) == 0)
            errors.Add("ESL flag missing");
        var masters = mod.ModHeader.MasterReferences.Select(x => x.Master.FileName.String).ToList();
        if (masters.Count != 1 || !masters[0].Equals("Skyrim.esm", StringComparison.OrdinalIgnoreCase))
            errors.Add("masters must be [Skyrim.esm], got [" + string.Join(", ", masters) + "]");
        if (mod.Armors.Count != 16) errors.Add($"expected 16 ARMO, got {mod.Armors.Count}");
        if (mod.ArmorAddons.Count != 16) errors.Add($"expected 16 ARMA, got {mod.ArmorAddons.Count}");

        for (var i = 0; i < 16; ++i) {
            var armorId = (uint)(0x800 + (i < 8 ? i : i + 8));
            var addonId = (uint)(0x808 + (i < 8 ? i : i + 8));
            var armor = mod.Armors.FirstOrDefault(x => x.FormKey.ID == armorId);
            var addon = mod.ArmorAddons.FirstOrDefault(x => x.FormKey.ID == addonId);
            if (armor == null || addon == null) {
                errors.Add($"missing pair ARMO {armorId:X3} / ARMA {addonId:X3}");
                continue;
            }
            var slot = i < 8 ? 44 : 54;
            var mask = (BipedObjectFlag)(1u << (slot - 30));
            if (armor.BodyTemplate.FirstPersonFlags != mask || addon.BodyTemplate.FirstPersonFlags != mask)
                errors.Add($"slot mismatch for pair {i + 1}");
            if (armor.Armature.Count != 1 || armor.Armature[0].FormKey != addon.FormKey)
                errors.Add($"armature link mismatch for pair {i + 1}");
            if (i < 8 && (armor.MajorFlags & Armor.MajorFlag.NonPlayable) != 0)
                errors.Add($"publish token {i + 1} is non-playable");
            if (i >= 8 && (armor.MajorFlags & Armor.MajorFlag.NonPlayable) == 0)
                errors.Add($"NPC persist token {i - 7} is playable");
            if (addon.Race.FormKey != new FormKey(ModKey.FromNameAndExtension("Skyrim.esm"), 0x19) ||
                    addon.AdditionalRaces.Count != kNpcRaceIds.Length)
                errors.Add($"race list mismatch for pair {i + 1}");
            if (addon.FirstPersonModel != null)
                errors.Add($"first-person model must be empty for pair {i + 1}");
            var number = i < 8 ? i + 1 : i - 7;
            var expectedArmorEdid = i < 8 ? $"CFW_PubToken{number:00}" : $"CFW_NpcPersistTok{number:00}";
            var expectedAddonEdid = i < 8 ? $"CFW_PubCarrier{number:00}" : $"CFW_NpcPersistCar{number:00}";
            if (armor.EditorID != expectedArmorEdid || addon.EditorID != expectedAddonEdid)
                errors.Add($"editor ID mismatch for pair {i + 1}");
            var expectedRaceKeys = kNpcRaceIds
                .Select(id => new FormKey(ModKey.FromNameAndExtension("Skyrim.esm"), id)).ToHashSet();
            if (!addon.AdditionalRaces.Select(x => x.FormKey).ToHashSet().SetEquals(expectedRaceKeys))
                errors.Add($"additional race set mismatch for pair {i + 1}");
            // RawPath = the string physically stored in the record. ToString()
            // normalizes to a Data-relative path (re-adding "Meshes\"), which
            // would mask a stored prefix - the exact defect this check hunts.
            var malePath = addon.WorldModel?.Male?.File?.RawPath?.Replace('/', '\\');
            var femalePath = addon.WorldModel?.Female?.File?.RawPath?.Replace('/', '\\');
            // Exact match, not EndsWith: a stray "Meshes\" prefix must FAIL here
            // (paths are Data\meshes-relative; EndsWith let the prefix through).
            if (i < 8) {
                var maleExpected = $"CostumeFW\\Pub{number:00}_carrier_m_r0.nif";
                var femaleExpected = $"CostumeFW\\Pub{number:00}_carrier_f_r0.nif";
                if (!string.Equals(malePath, maleExpected, StringComparison.OrdinalIgnoreCase) ||
                        !string.Equals(femalePath, femaleExpected, StringComparison.OrdinalIgnoreCase))
                    errors.Add($"publish model paths mismatch for pair {i + 1} (want exact, no meshes\\ prefix)");
            } else {
                var expected = $"CostumeFW\\NpcPersist{number:00}_carrier_r0.nif";
                if (!string.Equals(malePath, expected, StringComparison.OrdinalIgnoreCase) ||
                        !string.Equals(femalePath, expected, StringComparison.OrdinalIgnoreCase))
                    errors.Add($"NPC persist model paths mismatch for pair {i + 1} (want exact, no meshes\\ prefix)");
            }

        }

        foreach (var error in errors) Console.Error.WriteLine("VERIFY FAIL: " + error);
        if (errors.Count != 0) return 1;
        Console.WriteLine("verify NPC: 16 ARMO + 16 ARMA, ESL, Skyrim.esm-only, links/slots/races OK");
        return 0;
    }


    private static int Run(string basePath, string patchPath, string outDir, uint offset)
    {
        if (!File.Exists(basePath)) { Console.Error.WriteLine("missing base: " + basePath); return 1; }
        if (!File.Exists(patchPath)) { Console.Error.WriteLine("missing patch: " + patchPath); return 1; }

        var baseMod = SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(basePath), SkyrimRelease.SkyrimSE);
        var patchMod = SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(patchPath), SkyrimRelease.SkyrimSE);
        var outKey = ModKey.FromNameAndExtension(kOutName);

        // The PoC drop list belongs to the ORIGINAL v1.2.1 patch only - other
        // patches (e.g. VanillaSlots) legitimately use ids 0x806-0x808.
        var drop = patchMod.ModKey.FileName.String.StartsWith("CostumeFW_Boxes_FSMPCarrier",
                StringComparison.OrdinalIgnoreCase)
            ? new HashSet<FormKey>(kDropPatchIds.Select(id => new FormKey(patchMod.ModKey, id)))
            : new HashSet<FormKey>();

        // ---- collect winners ------------------------------------------------
        // base records (may be overridden by the patch), keyed by base FormKey
        var winners = new Dictionary<FormKey, IMajorRecordGetter>();
        foreach (var rec in baseMod.EnumerateMajorRecords()) {
            winners[rec.FormKey] = rec;
        }
        var patchNew = new List<IMajorRecordGetter>();
        int overrides = 0;
        foreach (var rec in patchMod.EnumerateMajorRecords()) {
            if (rec.FormKey.ModKey == baseMod.ModKey) {
                if (!winners.ContainsKey(rec.FormKey)) {
                    Console.Error.WriteLine("patch overrides unknown base record " + rec.FormKey);
                    return 1;
                }
                winners[rec.FormKey] = rec;  // patch override wins
                overrides++;
            } else if (rec.FormKey.ModKey == patchMod.ModKey) {
                if (!drop.Contains(rec.FormKey)) {
                    patchNew.Add(rec);
                }
            } else {
                Console.Error.WriteLine("patch overrides foreign record " + rec.FormKey + " - unexpected");
                return 1;
            }
        }

        // ---- build the FormKey remap ---------------------------------------
        var remap = new Dictionary<FormKey, FormKey>();
        foreach (var key in winners.Keys) {
            remap[key] = new FormKey(outKey, key.ID);  // base ids kept verbatim
        }
        foreach (var rec in patchNew) {
            remap[rec.FormKey] = new FormKey(outKey, rec.FormKey.ID + offset);
        }

        // renumbered ids must not collide with kept base ids
        var used = new HashSet<uint>(winners.Keys.Select(k => k.ID));
        foreach (var rec in patchNew) {
            var nid = rec.FormKey.ID + offset;
            if (!used.Add(nid)) {
                Console.Error.WriteLine($"renumber collision at {nid:X6} for {rec.FormKey}");
                return 1;
            }
        }

        // ---- duplicate into the merged mod ----------------------------------
        var outMod = new SkyrimMod(outKey, SkyrimRelease.SkyrimSE);
        // ESL flag (espfe). Raw 0x200 = LightMaster; enum member name varies
        // across Mutagen versions, the bit does not.
        outMod.ModHeader.Flags |= (SkyrimModHeader.HeaderFlag)0x200;

        var rows = new List<string>();
        foreach (var pair in winners.OrderBy(p => p.Key.ID)) {
            AddDuplicate(outMod, pair.Value, remap[pair.Key], rows);
        }
        foreach (var rec in patchNew.OrderBy(r => r.FormKey.ID)) {
            AddDuplicate(outMod, rec, remap[rec.FormKey], rows);
        }

        // remap every internal link (token ARMO -> carrier ARMA, etc.)
        foreach (var rec in outMod.EnumerateMajorRecords()) {
            rec.RemapLinks(remap);
        }

        // ---- write -----------------------------------------------------------
        Directory.CreateDirectory(outDir);
        var outPath = Path.Combine(outDir, kOutName);
        outMod.WriteToBinary(outPath);

        Console.WriteLine("merged -> " + outPath);
        Console.WriteLine($"  base records: {winners.Count} (of which {overrides} folded patch overrides)");
        Console.WriteLine($"  patch-new records carried: {patchNew.Count} (renumbered +0x{offset:X}), dropped: {drop.Count} (PoC)");
        foreach (var row in rows) {
            Console.WriteLine("  " + row);
        }

        // ---- verify (reopen what we actually wrote) --------------------------
        var check = SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(outPath), SkyrimRelease.SkyrimSE);
        int nRecords = 0;
        var badLinks = new List<string>();
        var oldKeys = new HashSet<ModKey> { baseMod.ModKey, patchMod.ModKey };
        // Re-merging INTO the same plugin name (v1.3.0: base already IS
        // CostumeFW.esp): links to the output key are the goal, not stale.
        oldKeys.Remove(outKey);
        foreach (var rec in check.EnumerateMajorRecords()) {
            nRecords++;
            if (rec.FormKey.ID > 0xFFF) {
                badLinks.Add($"formid out of ESL range: {rec.FormKey}");
            }
            foreach (var link in rec.EnumerateFormLinks()) {
                if (link.FormKey.IsNull) {
                    continue;
                }
                if (oldKeys.Contains(link.FormKey.ModKey)) {
                    badLinks.Add($"{rec.FormKey} ({rec.EditorID}) still links {link.FormKey}");
                }
            }
        }
        var light = (check.ModHeader.Flags & (SkyrimModHeader.HeaderFlag)0x200) != 0;
        var masters = check.ModHeader.MasterReferences.Select(m => m.Master.FileName.ToString()).ToList();
        Console.WriteLine($"verify: {nRecords} record(s), light={light}, masters=[{string.Join(", ", masters)}]");
        if (!light) {
            badLinks.Add("light (ESL) flag missing");
        }
        if (badLinks.Count > 0) {
            foreach (var b in badLinks) {
                Console.Error.WriteLine("VERIFY FAIL: " + b);
            }
            return 1;
        }

        // ---- SEQ (StartGameEnabled quest must be listed to start mid-save) ---
        // raw in-file formid = (master count << 24) | local id
        var seqDir = Path.Combine(outDir, "SEQ");
        Directory.CreateDirectory(seqDir);
        var raw = ((uint)masters.Count << 24) | kMcmQuestLocalId;
        var seqPath = Path.Combine(seqDir, Path.GetFileNameWithoutExtension(kOutName) + ".seq");
        File.WriteAllBytes(seqPath, BitConverter.GetBytes(raw));
        Console.WriteLine($"seq: {seqPath} (quest {raw:X8})");

        Console.WriteLine("OK");
        return 0;
    }

    private static void AddDuplicate(
        SkyrimMod outMod, IMajorRecordGetter rec, FormKey newKey, List<string> rows)
    {
        switch (rec) {
            case IArmorGetter a: outMod.Armors.RecordCache.Set(a.Duplicate(newKey)); break;
            case IArmorAddonGetter aa: outMod.ArmorAddons.RecordCache.Set(aa.Duplicate(newKey)); break;
            case IQuestGetter q: outMod.Quests.RecordCache.Set(q.Duplicate(newKey)); break;
            case ISpellGetter s: outMod.Spells.RecordCache.Set(s.Duplicate(newKey)); break;
            case IContainerGetter c: outMod.Containers.RecordCache.Set(c.Duplicate(newKey)); break;
            case IHeadPartGetter h: outMod.HeadParts.RecordCache.Set(h.Duplicate(newKey)); break;
            default:
                throw new InvalidOperationException(
                    $"unhandled record type {rec.GetType().Name} at {rec.FormKey}");
        }
        rows.Add($"{rec.FormKey.ID:X6} -> {newKey.ID:X6}  {rec.GetType().Name,-24} {rec.EditorID}");
    }
}

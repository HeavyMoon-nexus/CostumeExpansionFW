// espmerge - one-shot v1.2.1 plugin consolidation for Costume Expansion FW.
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
        var basePath = args.Length > 0 ? args[0] : kDefaultBase;
        var patchPath = args.Length > 1 ? args[1] : kDefaultPatch;
        var outDir = args.Length > 2 ? args[2] : kDefaultOutDir;

        try {
            return Run(basePath, patchPath, outDir);
        } catch (Exception ex) {
            Console.Error.WriteLine("FATAL: " + ex);
            return 1;
        }
    }

    private static int Run(string basePath, string patchPath, string outDir)
    {
        if (!File.Exists(basePath)) { Console.Error.WriteLine("missing base: " + basePath); return 1; }
        if (!File.Exists(patchPath)) { Console.Error.WriteLine("missing patch: " + patchPath); return 1; }

        var baseMod = SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(basePath), SkyrimRelease.SkyrimSE);
        var patchMod = SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(patchPath), SkyrimRelease.SkyrimSE);
        var outKey = ModKey.FromNameAndExtension(kOutName);

        var drop = new HashSet<FormKey>(
            kDropPatchIds.Select(id => new FormKey(patchMod.ModKey, id)));

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
            remap[rec.FormKey] = new FormKey(outKey, rec.FormKey.ID + kRenumberOffset);
        }

        // renumbered ids must not collide with kept base ids
        var used = new HashSet<uint>(winners.Keys.Select(k => k.ID));
        foreach (var rec in patchNew) {
            var nid = rec.FormKey.ID + kRenumberOffset;
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
        Console.WriteLine($"  patch-new records carried: {patchNew.Count} (renumbered +0x{kRenumberOffset:X}), dropped: {kDropPatchIds.Length} (PoC)");
        foreach (var row in rows) {
            Console.WriteLine("  " + row);
        }

        // ---- verify (reopen what we actually wrote) --------------------------
        var check = SkyrimMod.CreateFromBinaryOverlay(ModPath.FromPath(outPath), SkyrimRelease.SkyrimSE);
        int nRecords = 0;
        var badLinks = new List<string>();
        var oldKeys = new HashSet<ModKey> { baseMod.ModKey, patchMod.ModKey };
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

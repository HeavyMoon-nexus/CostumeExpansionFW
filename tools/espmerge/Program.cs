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
        if (args.Length >= 2 && args[0] == "--verify-core") {
            try { return VerifyCore(args[1]); }
            catch (Exception ex) { Console.Error.WriteLine("FATAL: " + ex); return 1; }
        }
        if (args.Length >= 2 && args[0] == "--add-marker") {
            try { return AddBoxTokenMarker(args[1]); }
            catch (Exception ex) { Console.Error.WriteLine("FATAL: " + ex); return 1; }
        }
        if (args.Length >= 4 && args[0] == "--build-boxpool") {
            try {
                var perSlot = args.Length > 4 ? int.Parse(args[4]) : kDefaultPerSlot;
                return BuildBoxPool(int.Parse(args[1]), args[2], args[3], perSlot);
            } catch (Exception ex) { Console.Error.WriteLine("FATAL: " + ex); return 1; }
        }
        if (args.Length >= 3 && args[0] == "--verify-abilities") {
            try { return VerifyAbilityPool(int.Parse(args[1]), args[2]); }
            catch (Exception ex) { Console.Error.WriteLine("FATAL: " + ex); return 1; }
        }
        if (args.Length >= 3 && args[0] == "--verify-pool") {
            try { return VerifyBoxPool(args[1], args[2]); }
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

    // The community-standard additional-race set (RNAM stays DefaultRace).
    // MUST stay in sync with tools/esprace (main, commit 01277f6): custom races
    // whose RACE.ArmorRace proxies a vanilla race only render a worn ARMA when
    // that vanilla race is in this list - a gap means the invisible carrier
    // silently never attaches and FSMP gets no mesh (permanently static).
    private static readonly uint[] kNpcRaceIds = {
        0x013740, // ArgonianRace
        0x013741, // BretonRace
        0x013742, // DarkElfRace
        0x013743, // HighElfRace
        0x013744, // ImperialRace
        0x013745, // KhajiitRace
        0x013746, // NordRace
        0x013747, // OrcRace
        0x013748, // RedguardRace
        0x013749, // WoodElfRace
        0x067CD8, // ElderRace
        0x088794, // NordRaceVampire
        0x08883A, // ArgonianRaceVampire
        0x08883C, // BretonRaceVampire
        0x08883D, // DarkElfRaceVampire
        0x088840, // HighElfRaceVampire
        0x088844, // ImperialRaceVampire
        0x088845, // KhajiitRaceVampire
        0x088846, // RedguardRaceVampire
        0x088884, // WoodElfRaceVampire
        0x0A82B9, // OrcRaceVampire
        0x0A82BA, // ElderRaceVampire
        0x10760A, // ManakinRace
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

    // --- v1.6.4 generation 0: the box-token marker -------------------------
    // Membership in the box token pool stopped being a question about a display
    // name in 1.6.4 (a token's FULL is rewritten at runtime and can be
    // translated). It is a keyword now, and this is the one place that puts it
    // on the 27 tokens that shipped before it existed.
    private const string kMarkerEdid = "CFW_BoxTokenMarker";
    private const string kBoxTokenEdidPrefix = "CFW_BoxToken_";
    private const int kCoreArmorCount = 27;
    private const int kCoreAddonCount = 31;
    private const float kHeaderVersion = 1.71f;

    // Every ARMO whose editor ID is CFW_BoxToken_<slot>. The ARMA records are
    // named ..._<slot>AA, so the all-digits tail is what keeps them out.
    private static bool IsBoxTokenEdid(string edid)
    {
        if (edid == null || !edid.StartsWith(kBoxTokenEdidPrefix, StringComparison.Ordinal))
            return false;
        var tail = edid.Substring(kBoxTokenEdidPrefix.Length);
        return tail.Length > 0 && tail.All(char.IsDigit);
    }

    private static int SlotOfMask(uint mask)
    {
        for (var bit = 0; bit < 32; ++bit)
            if ((mask & (1u << bit)) != 0) return bit + 30;
        return 0;
    }

    private static int AddBoxTokenMarker(string path)
    {
        if (!File.Exists(path)) {
            Console.Error.WriteLine("missing core plugin: " + path);
            return 1;
        }
        var mod = SkyrimMod.CreateFromBinary(ModPath.FromPath(path), SkyrimRelease.SkyrimSE);
        var modKey = mod.ModKey;

        var marker = mod.Keywords.FirstOrDefault(x => x.EditorID == kMarkerEdid);
        if (marker == null) {
            // Append past the used range; never renumber an existing record.
            // Local ids are published identity - the settings file, the co-save
            // and every shared preset name records by FormID.
            uint next = 0x800;
            foreach (var rec in mod.EnumerateMajorRecords())
                if (rec.FormKey.ModKey == modKey && rec.FormKey.ID >= next)
                    next = rec.FormKey.ID + 1;
            if (next > 0xFFF) {
                Console.Error.WriteLine("no room for the marker: next free id " + next.ToString("X") +
                                        " is past the ESL range");
                return 1;
            }
            marker = new Keyword(new FormKey(modKey, next), SkyrimRelease.SkyrimSE) {
                EditorID = kMarkerEdid,
            };
            mod.Keywords.RecordCache.Set(marker);
            Console.WriteLine("created KYWD " + kMarkerEdid + " at " + next.ToString("X6"));
        } else {
            Console.WriteLine("KYWD " + kMarkerEdid + " already present at " +
                              marker.FormKey.ID.ToString("X6"));
        }

        var tagged = 0;
        var already = 0;
        foreach (var armor in mod.Armors.ToList()) {
            if (!IsBoxTokenEdid(armor.EditorID)) continue;
            if (armor.Keywords == null)
                armor.Keywords = new Noggog.ExtendedList<IFormLinkGetter<IKeywordGetter>>();
            if (armor.Keywords.Any(x => x.FormKey == marker.FormKey)) { ++already; continue; }
            armor.Keywords.Add(new FormLink<IKeywordGetter>(marker.FormKey));
            ++tagged;
        }
        Console.WriteLine("marker on " + tagged + " newly tagged + " + already +
                          " already tagged box token(s)");
        if (tagged == 0 && already == 0) {
            Console.Error.WriteLine("no CFW_BoxToken_* ARMO found - wrong file?");
            return 1;
        }

        // Write beside the original and swap only once the result verifies, so a
        // half-written plugin never replaces a shipping one. The staging copy
        // keeps the REAL file name in a scratch directory rather than picking up
        // a .tmp suffix: Mutagen resolves the output path to a ModKey to check
        // its masters, and "CostumeFW.esp.tmp" is not a plugin name.
        var dir = Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".";
        var stageDir = Path.Combine(dir, ".espmerge_stage");
        Directory.CreateDirectory(stageDir);
        var staged = Path.Combine(stageDir, Path.GetFileName(path));
        if (File.Exists(staged)) File.Delete(staged);
        mod.WriteToBinary(staged);
        if (VerifyCore(staged) != 0) {
            Console.Error.WriteLine("the rewritten plugin does not verify - original untouched. " +
                                    "The rejected result is at " + staged);
            return 1;
        }
        File.Copy(staged, path, true);
        File.Delete(staged);
        try { Directory.Delete(stageDir); } catch (IOException) { /* not empty: leave it */ }
        Console.WriteLine("add-marker -> " + path);
        return 0;
    }

    private static int VerifyCore(string path)
    {
        if (!File.Exists(path)) {
            Console.Error.WriteLine("missing core plugin: " + path);
            return 1;
        }
        // using: the overlay memory-maps the file and HOLDS it. Without this the
        // staged copy in --add-marker cannot be deleted after it verifies.
        using var mod = SkyrimMod.CreateFromBinaryOverlay(
            ModPath.FromPath(path), SkyrimRelease.SkyrimSE);
        var errors = new List<string>();

        if ((mod.ModHeader.Flags & (SkyrimModHeader.HeaderFlag)0x200) == 0)
            errors.Add("ESL flag missing");
        if (Math.Abs(mod.ModHeader.Stats.Version - kHeaderVersion) > 0.001f)
            errors.Add("HEDR version must be " + kHeaderVersion + ", got " + mod.ModHeader.Stats.Version);
        var masters = mod.ModHeader.MasterReferences.Select(x => x.Master.FileName.String).ToList();
        if (masters.Count != 1 || !masters[0].Equals("Skyrim.esm", StringComparison.OrdinalIgnoreCase))
            errors.Add("masters must be [Skyrim.esm], got [" + string.Join(", ", masters) + "]");

        foreach (var rec in mod.EnumerateMajorRecords()) {
            if (rec.FormKey.ModKey != mod.ModKey) continue;
            if (rec.FormKey.ID < 0x800 || rec.FormKey.ID > 0xFFF)
                errors.Add("local id " + rec.FormKey.ID.ToString("X6") + " (" + rec.EditorID +
                           ") is outside the ESL range 800-FFF");
        }

        var marker = mod.Keywords.FirstOrDefault(x => x.EditorID == kMarkerEdid);
        if (marker == null) errors.Add("KYWD " + kMarkerEdid + " is missing");

        var armors = mod.Armors.ToList();
        var addons = mod.ArmorAddons.ToList();
        if (armors.Count != kCoreArmorCount)
            errors.Add("expected " + kCoreArmorCount + " ARMO, got " + armors.Count);
        if (addons.Count != kCoreAddonCount)
            errors.Add("expected " + kCoreAddonCount + " ARMA, got " + addons.Count);

        var skyrimKey = ModKey.FromNameAndExtension("Skyrim.esm");
        var expectedRaces = kNpcRaceIds.Select(id => new FormKey(skyrimKey, id)).ToHashSet();

        foreach (var armor in armors) {
            var who = armor.EditorID ?? armor.FormKey.ID.ToString("X6");
            if (!IsBoxTokenEdid(armor.EditorID)) {
                // The pool is "every ARMO this plugin defines", so a non-token
                // ARMO landing here would silently join it.
                errors.Add("ARMO " + who + " is not a CFW_BoxToken_<slot> record");
                continue;
            }
            if (marker != null &&
                (armor.Keywords == null || !armor.Keywords.Any(k => k.FormKey == marker.FormKey)))
                errors.Add(who + " does not carry " + kMarkerEdid);

            if (armor.Armature.Count != 1) {
                errors.Add(who + " references " + armor.Armature.Count + " ARMA (want exactly 1)");
                continue;
            }
            var addon = addons.FirstOrDefault(a => a.FormKey == armor.Armature[0].FormKey);
            if (addon == null) { errors.Add(who + " points at an ARMA outside this plugin"); continue; }

            var armorMask = (uint)armor.BodyTemplate.FirstPersonFlags;
            var addonMask = (uint)addon.BodyTemplate.FirstPersonFlags;
            // NOT "exactly one bit": the shipped slot-31 wig token carries 31|41
            // (Hair + LongHair) and that is correct. What must hold is that the
            // pair agree and that the slot is real.
            if (armorMask == 0) errors.Add(who + " has an empty BOD2");
            if (armorMask != addonMask)
                errors.Add(who + " BOD2 " + armorMask.ToString("X") + " != its ARMA's " +
                           addonMask.ToString("X"));
            var slot = SlotOfMask(armorMask);
            if (slot < 30 || slot > 61) errors.Add(who + " slot " + slot + " is outside 30-61");
            var edidSlot = int.Parse(armor.EditorID.Substring(kBoxTokenEdidPrefix.Length));
            if (slot != edidSlot)
                errors.Add(who + " names slot " + edidSlot + " but its BOD2 says " + slot);

            // The 23-race list is the fix for "a box silently does nothing on an
            // ArmorRace custom race"; a generated pool that loses it would
            // reproduce that bug for its own boxes only.
            if (addon.Race.FormKey != new FormKey(skyrimKey, 0x19))
                errors.Add(addon.EditorID + " RNAM is not DefaultRace");
            if (!addon.AdditionalRaces.Select(x => x.FormKey).ToHashSet().SetEquals(expectedRaces))
                errors.Add(addon.EditorID + " additional-race list does not match the 23-race set");
        }

        foreach (var error in errors) Console.Error.WriteLine("VERIFY FAIL: " + error);
        if (errors.Count != 0) return 1;
        Console.WriteLine("verify core: " + armors.Count + " ARMO + " + addons.Count +
                          " ARMA, ESL, HEDR " + kHeaderVersion +
                          ", Skyrim.esm-only, marker/BOD2/ARMA-link/races OK");
        return 0;
    }

    // --- v1.6.4 BOX pool generations ---------------------------------------
    // CostumeFW_BoxPoolN.esp adds more physical box tokens, so several boxes can
    // share one biped slot. Every record is DERIVED from the matching generation
    // 0 token rather than built from a slot number, because gen 0 carries two
    // things that are easy to get wrong and invisible when wrong:
    //
    //   the BOD2 mask   - slot 31's token is 31|41 (Hair + LongHair), not one bit
    //   the race list   - RNAM plus 23 additional races. Without them a box
    //                     silently does nothing on an ArmorRace custom race, the
    //                     exact bug tools/esprace was written to fix
    //
    // Copying the template keeps both correct by construction; --verify-core and
    // --verify-pool then assert they stayed that way.
    private const int kDefaultPerSlot = 3;

    private static string CarrierKey(int generation, uint armoLocalId) =>
        "BP" + generation.ToString("00") + "_" + armoLocalId.ToString("X6");

    private static int BuildBoxPool(int generation, string corePath, string outDir, int perSlot)
    {
        if (generation < 1) {
            Console.Error.WriteLine("generation must be >= 1 (generation 0 is CostumeFW.esp)");
            return 1;
        }
        if (perSlot < 1) {
            Console.Error.WriteLine("per-slot count must be >= 1");
            return 1;
        }
        if (!File.Exists(corePath)) {
            Console.Error.WriteLine("missing core plugin: " + corePath);
            return 1;
        }
        using var core = SkyrimMod.CreateFromBinaryOverlay(
            ModPath.FromPath(corePath), SkyrimRelease.SkyrimSE);

        var marker = core.Keywords.FirstOrDefault(x => x.EditorID == kMarkerEdid);
        if (marker == null) {
            Console.Error.WriteLine("the core plugin has no " + kMarkerEdid +
                                    " - run --add-marker on it first");
            return 1;
        }

        // Templates in slot order, so the generated ids are stable across runs.
        var templates = core.Armors
            .Where(x => IsBoxTokenEdid(x.EditorID))
            .Select(x => new {
                Armor = x,
                Slot = int.Parse(x.EditorID.Substring(kBoxTokenEdidPrefix.Length)),
            })
            .OrderBy(x => x.Slot)
            .ToList();
        if (templates.Count != kCoreArmorCount) {
            Console.Error.WriteLine("expected " + kCoreArmorCount + " gen-0 tokens, found " +
                                    templates.Count);
            return 1;
        }

        var outName = "CostumeFW_BoxPool" + generation + ".esp";
        var outKey = ModKey.FromNameAndExtension(outName);
        var outMod = new SkyrimMod(outKey, SkyrimRelease.SkyrimSE);
        outMod.ModHeader.Flags |= (SkyrimModHeader.HeaderFlag)0x200;  // ESL
        outMod.ModHeader.Stats.Version = kHeaderVersion;

        var kid = new List<string>();
        var index = 0;
        foreach (var t in templates) {
            var addonTemplate = core.ArmorAddons.FirstOrDefault(
                a => t.Armor.Armature.Count == 1 && a.FormKey == t.Armor.Armature[0].FormKey);
            if (addonTemplate == null) {
                Console.Error.WriteLine("gen-0 token " + t.Armor.EditorID + " has no single ARMA");
                return 1;
            }
            for (var ord = 1; ord <= perSlot; ++ord, ++index) {
                var armoId = (uint)(0x800 + index * 2);
                var armaId = armoId + 1;
                if (armaId > 0xFFF) {
                    Console.Error.WriteLine("this generation does not fit the ESL range - " +
                                            "lower --per-slot or add another generation");
                    return 1;
                }
                var armor = t.Armor.Duplicate(new FormKey(outKey, armoId));
                var addon = addonTemplate.Duplicate(new FormKey(outKey, armaId));

                var tag = "CFW_BP" + generation.ToString("00") + "_S" + t.Slot + "_" +
                          ord.ToString("00");
                armor.EditorID = tag;
                addon.EditorID = tag + "_AA";
                // The FULL must NOT start with "Costume Box". A 1.6.3 DLL finds
                // box tokens by "a CostumeFW* plugin AND that name prefix", and
                // BoxPoolN satisfies the first half - so keeping the prefix off
                // means a user who rolls back to 1.6.3 cannot be handed a pool
                // token and build an unguarded same-slot box with it. At runtime
                // CEF overwrites this with the box's label anyway; it is visible
                // only in the free-token picker and diagnostics.
                armor.Name = "CFW Pool " + generation + " Slot " + t.Slot + " #" + ord;

                armor.Armature.Clear();
                armor.Armature.Add(new FormLink<IArmorAddonGetter>(addon.FormKey));

                // Duplicate carried the marker over with the rest of the keywords;
                // assert rather than assume, since it is the membership test.
                if (armor.Keywords == null ||
                    !armor.Keywords.Any(k => k.FormKey == marker.FormKey)) {
                    Console.Error.WriteLine("template " + t.Armor.EditorID +
                                            " did not carry the marker into " + tag);
                    return 1;
                }

                // One carrier namespace per TOKEN, not per slot: several tokens
                // share slot 55 now, and slot-keyed file names would have them
                // overwrite each other's meshes.
                var key = CarrierKey(generation, armoId);
                var nif = "CostumeFW\\" + key + "_carrier_r0.nif";
                addon.WorldModel.Male.File = nif;
                addon.WorldModel.Female.File = nif;

                outMod.Armors.RecordCache.Set(armor);
                outMod.ArmorAddons.RecordCache.Set(addon);

                // LoreBox tooltips are keyed per token from this generation on;
                // gen 0 keeps its LoreBox_CEFBox<slot> names as slot aliases.
                kid.Add("Keyword = LoreBox_CEFTok_" + key + "|Armor|0x" +
                        armoId.ToString("X") + "~" + outName);
            }
        }

        Directory.CreateDirectory(outDir);
        var outPath = Path.Combine(outDir, outName);
        outMod.WriteToBinary(outPath);

        var kidPath = Path.Combine(outDir, "CostumeFW_BoxPool" + generation + "_KID.ini");
        var header = new List<string> {
            "; Costume Expansion FW - LoreBox tooltip keywords for box pool generation " +
                generation + ".",
            "; Generated by tools/espmerge --build-boxpool; do not edit by hand.",
            ";",
            "; One keyword per TOKEN (LoreBox_CEFTok_<carrierKey>), not per slot: a slot",
            "; can hold several boxes from 1.6.4 on, and a slot-keyed tooltip would show",
            "; whichever of them CEF happened to find first.",
            ";",
            "; Soft dependency: without \"LoreBox - Item and Spell Tooltips\" nothing reads",
            "; these and there is no effect. KID auto-creates the keywords.",
            "",
        };
        File.WriteAllLines(kidPath, header.Concat(kid));

        Console.WriteLine("box pool " + generation + " -> " + outPath);
        Console.WriteLine("  " + outMod.Armors.Count + " ARMO + " + outMod.ArmorAddons.Count +
                          " ARMA (" + perSlot + " per slot x " + templates.Count + " slots)");
        Console.WriteLine("  " + kidPath + " (" + kid.Count + " keyword lines)");
        return VerifyBoxPool(outPath, corePath);
    }

    private static int VerifyBoxPool(string path, string corePath)
    {
        if (!File.Exists(path)) {
            Console.Error.WriteLine("missing pool plugin: " + path);
            return 1;
        }
        using var mod = SkyrimMod.CreateFromBinaryOverlay(
            ModPath.FromPath(path), SkyrimRelease.SkyrimSE);
        using var core = SkyrimMod.CreateFromBinaryOverlay(
            ModPath.FromPath(corePath), SkyrimRelease.SkyrimSE);
        var errors = new List<string>();

        var generation = 0;
        var name = Path.GetFileName(path);
        var stem = "CostumeFW_BoxPool";
        if (name.StartsWith(stem, StringComparison.OrdinalIgnoreCase) &&
            name.EndsWith(".esp", StringComparison.OrdinalIgnoreCase)) {
            var digits = name.Substring(stem.Length, name.Length - stem.Length - 4);
            if (digits.Length > 0 && digits[0] != '0' && digits.All(char.IsDigit))
                generation = int.Parse(digits);
        }
        if (generation < 1)
            errors.Add("file name " + name + " is not CostumeFW_BoxPool<N>.esp with N >= 1");

        if ((mod.ModHeader.Flags & (SkyrimModHeader.HeaderFlag)0x200) == 0)
            errors.Add("ESL flag missing");
        if (Math.Abs(mod.ModHeader.Stats.Version - kHeaderVersion) > 0.001f)
            errors.Add("HEDR version must be " + kHeaderVersion + ", got " + mod.ModHeader.Stats.Version);

        // Masters are FIXED and checked for exact equality. The cumulative chain
        // across pool generations was withdrawn in rev.5: a missing master is a
        // startup CTD in SE, which takes the diagnostic CEF would otherwise print
        // away from the user entirely.
        var masters = mod.ModHeader.MasterReferences
            .Select(x => x.Master.FileName.String).ToList();
        var wanted = new[] { "Skyrim.esm", "CostumeFW.esp" };
        if (masters.Count != wanted.Length ||
            !masters.Select(m => m.ToLowerInvariant()).OrderBy(m => m)
                .SequenceEqual(wanted.Select(m => m.ToLowerInvariant()).OrderBy(m => m)))
            errors.Add("masters must be exactly [" + string.Join(", ", wanted) + "], got [" +
                       string.Join(", ", masters) + "]");

        var marker = core.Keywords.FirstOrDefault(x => x.EditorID == kMarkerEdid);
        if (marker == null) errors.Add("the core plugin has no " + kMarkerEdid);

        var armors = mod.Armors.ToList();
        var addons = mod.ArmorAddons.ToList();
        if (armors.Count != addons.Count)
            errors.Add("ARMO/ARMA counts differ: " + armors.Count + " vs " + addons.Count);

        // gen 0 keyed by slot, to compare masks and race lists against.
        var coreBySlot = core.Armors
            .Where(x => IsBoxTokenEdid(x.EditorID))
            .ToDictionary(x => int.Parse(x.EditorID.Substring(kBoxTokenEdidPrefix.Length)));

        var carrierKeys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var armor in armors) {
            var who = armor.EditorID ?? armor.FormKey.ID.ToString("X6");
            if (armor.FormKey.ID < 0x800 || armor.FormKey.ID > 0xFFF)
                errors.Add(who + " local id " + armor.FormKey.ID.ToString("X6") +
                           " is outside the ESL range 800-FFF");
            if (armor.Name != null &&
                armor.Name.String != null &&
                armor.Name.String.StartsWith("Costume Box", StringComparison.OrdinalIgnoreCase))
                errors.Add(who + " FULL starts with \"Costume Box\" - a 1.6.3 DLL would hand it out");
            if (marker != null &&
                (armor.Keywords == null || !armor.Keywords.Any(k => k.FormKey == marker.FormKey)))
                errors.Add(who + " does not carry " + kMarkerEdid);
            if (armor.Armature.Count != 1) {
                errors.Add(who + " references " + armor.Armature.Count + " ARMA (want exactly 1)");
                continue;
            }
            var addon = addons.FirstOrDefault(a => a.FormKey == armor.Armature[0].FormKey);
            if (addon == null) { errors.Add(who + " points at an ARMA outside this plugin"); continue; }

            var mask = (uint)armor.BodyTemplate.FirstPersonFlags;
            if (mask == 0) errors.Add(who + " has an empty BOD2");
            if (mask != (uint)addon.BodyTemplate.FirstPersonFlags)
                errors.Add(who + " BOD2 does not match its ARMA");
            var slot = SlotOfMask(mask);
            if (!coreBySlot.TryGetValue(slot, out var gen0)) {
                errors.Add(who + " slot " + slot + " has no generation 0 token");
            } else if (mask != (uint)gen0.BodyTemplate.FirstPersonFlags) {
                // slot 31 is the one this catches: 31|41, not a single bit.
                errors.Add(who + " BOD2 " + mask.ToString("X") + " differs from generation 0's " +
                           ((uint)gen0.BodyTemplate.FirstPersonFlags).ToString("X") +
                           " for slot " + slot);
            } else {
                var gen0Addon = core.ArmorAddons.FirstOrDefault(
                    a => gen0.Armature.Count == 1 && a.FormKey == gen0.Armature[0].FormKey);
                if (gen0Addon != null) {
                    if (addon.Race.FormKey != gen0Addon.Race.FormKey)
                        errors.Add(addon.EditorID + " RNAM differs from generation 0's");
                    if (!addon.AdditionalRaces.Select(x => x.FormKey).ToHashSet()
                            .SetEquals(gen0Addon.AdditionalRaces.Select(x => x.FormKey)))
                        errors.Add(addon.EditorID + " additional-race list differs from generation 0's");
                }
            }

            var key = CarrierKey(generation, armor.FormKey.ID);
            if (!carrierKeys.Add(key)) errors.Add("duplicate carrier key " + key);
            var expected = "CostumeFW\\" + key + "_carrier_r0.nif";
            var male = addon.WorldModel?.Male?.File?.RawPath?.Replace('/', '\\');
            var female = addon.WorldModel?.Female?.File?.RawPath?.Replace('/', '\\');
            if (!string.Equals(male, expected, StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(female, expected, StringComparison.OrdinalIgnoreCase))
                errors.Add(addon.EditorID + " model path is not " + expected +
                           " (got " + male + " / " + female + ")");
        }

        // The KID file beside it must name every token exactly once.
        var kidPath = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".",
            "CostumeFW_BoxPool" + generation + "_KID.ini");
        if (!File.Exists(kidPath)) {
            errors.Add("missing " + Path.GetFileName(kidPath));
        } else {
            var lines = File.ReadAllLines(kidPath)
                .Where(l => l.StartsWith("Keyword =", StringComparison.Ordinal)).ToList();
            if (lines.Count != armors.Count)
                errors.Add("KID ini has " + lines.Count + " keyword lines for " + armors.Count +
                           " token(s)");
            foreach (var armor in armors) {
                var key = CarrierKey(generation, armor.FormKey.ID);
                var want = "Keyword = LoreBox_CEFTok_" + key + "|Armor|0x" +
                           armor.FormKey.ID.ToString("X") + "~" + Path.GetFileName(path);
                if (!lines.Contains(want)) errors.Add("KID ini is missing: " + want);
            }
        }

        foreach (var error in errors) Console.Error.WriteLine("VERIFY FAIL: " + error);
        if (errors.Count != 0) return 1;
        Console.WriteLine("verify pool " + generation + ": " + armors.Count + " ARMO + " +
                          addons.Count + " ARMA, ESL, HEDR " + kHeaderVersion +
                          ", masters/marker/BOD2-vs-gen0/races/carrier-keys/KID OK");
        return 0;
    }

    // --- v1.6.4 ability pool generations -----------------------------------
    // tools/make_ability_pool.py writes these; this says whether what it wrote
    // is what the runtime will accept. The two halves that matter are the ones
    // a mistake makes invisible until a save is already pointing into the file:
    // the ESL flag (generation 1 is NOT ESL and can never become one; every
    // later generation must be, or it burns a load-order slot for nothing), and
    // a CONTIGUOUS run of local ids from 0x800 - because the runtime measures a
    // generation's size by walking until an id stops resolving, so a gap at
    // 0x8xx silently shortens the pool to that point.
    private static int VerifyAbilityPool(int generation, string path)
    {
        if (generation < 1) {
            Console.Error.WriteLine("generation must be >= 1");
            return 1;
        }
        if (!File.Exists(path)) {
            Console.Error.WriteLine("missing ability pool: " + path);
            return 1;
        }
        using var mod = SkyrimMod.CreateFromBinaryOverlay(
            ModPath.FromPath(path), SkyrimRelease.SkyrimSE);
        var errors = new List<string>();

        var wantName = generation == 1 ? "CostumeFW_Abilities.esp"
                                       : "CostumeFW_Abilities" + generation + ".esp";
        if (!Path.GetFileName(path).Equals(wantName, StringComparison.OrdinalIgnoreCase))
            errors.Add("generation " + generation + " must be named " + wantName + ", got " +
                       Path.GetFileName(path));

        var esl = (mod.ModHeader.Flags & (SkyrimModHeader.HeaderFlag)0x200) != 0;
        if (generation == 1 && esl)
            errors.Add("generation 1 shipped WITHOUT the ESL flag and cannot gain it - the flag " +
                       "changes how every form in the file is addressed");
        if (generation >= 2 && !esl)
            errors.Add("generation " + generation + " must carry the ESL flag");

        if (Math.Abs(mod.ModHeader.Stats.Version - kHeaderVersion) > 0.001f)
            errors.Add("HEDR version must be " + kHeaderVersion + ", got " +
                       mod.ModHeader.Stats.Version);

        var masters = mod.ModHeader.MasterReferences.Select(x => x.Master.FileName.String).ToList();
        if (masters.Count != 1 || !masters[0].Equals("Skyrim.esm", StringComparison.OrdinalIgnoreCase))
            errors.Add("masters must be [Skyrim.esm], got [" + string.Join(", ", masters) + "]");

        var spells = mod.Spells.ToList();
        var others = mod.EnumerateMajorRecords()
            .Where(x => x.FormKey.ModKey == mod.ModKey)
            .Count() - spells.Count;
        if (others != 0)
            errors.Add("the pool must hold SPEL records only, found " + others + " other record(s)");

        var wantCount = generation == 1 ? 1024 : 2048;
        if (spells.Count != wantCount)
            errors.Add("generation " + generation + " must hold " + wantCount + " abilities, got " +
                       spells.Count);

        var ids = spells.Select(x => x.FormKey.ID).OrderBy(x => x).ToList();
        for (var i = 0; i < ids.Count; i++) {
            var want = (uint)(0x800 + i);
            if (ids[i] != want) {
                errors.Add("local ids must run contiguously from 000800; expected " +
                           want.ToString("X6") + " at position " + i + ", got " +
                           ids[i].ToString("X6"));
                break;
            }
        }
        if (ids.Count > 0 && ids[ids.Count - 1] > 0xFFF)
            errors.Add("local id " + ids[ids.Count - 1].ToString("X6") +
                       " is outside the light range 800-FFF");

        foreach (var spell in spells) {
            var want = "CFW_Ability_" + (spell.FormKey.ID - 0x800).ToString("0000");
            if (spell.EditorID != want)
                errors.Add(spell.FormKey.ID.ToString("X6") + " EditorID is '" + spell.EditorID +
                           "', expected '" + want + "'");
            if (spell.Type != SpellType.Ability)
                errors.Add(want + " is not an Ability spell");
            if (spell.CastType != CastType.ConstantEffect)
                errors.Add(want + " is not constant-effect");
            if (spell.TargetType != TargetType.Self)
                errors.Add(want + " does not target self");
            if (spell.Effects.Count != 0)
                errors.Add(want + " ships with " + spell.Effects.Count + " effect(s) - the pool " +
                           "is empty by design and CEF fills it at kDataLoaded");
        }

        foreach (var error in errors) Console.Error.WriteLine("VERIFY FAIL: " + error);
        if (errors.Count != 0) return 1;
        Console.WriteLine("verify abilities " + generation + ": " + spells.Count +
                          " SPEL, " + (esl ? "ESL" : "non-ESL") + ", HEDR " + kHeaderVersion +
                          ", Skyrim.esm-only, ids/EDID/type/empty-effects OK");
        return 0;
    }

    private static int VerifyNpcAddon(string path)
    {
        if (!File.Exists(path)) {
            Console.Error.WriteLine("missing NPC add-on: " + path);
            return 1;
        }
        // using: the overlay memory-maps the file and HOLDS it. Without this the
        // staged copy in --add-marker cannot be deleted after it verifies.
        using var mod = SkyrimMod.CreateFromBinaryOverlay(
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

        // Role split. This add-on and the core both pass "is it a CEF plugin",
        // so what keeps a publish token out of the box pool is that it is not a
        // box token: no CFW_BoxToken_* editor ID, and no box-token marker. Both
        // are asserted here rather than left as a naming convention, because the
        // convention is what failed - a display name is not identity.
        foreach (var armor in mod.Armors) {
            if (IsBoxTokenEdid(armor.EditorID))
                errors.Add("ARMO " + armor.EditorID + " uses a box-token editor ID - it would " +
                           "read as a box token");
            if (armor.Keywords != null && armor.Keywords.Count != 0)
                errors.Add("ARMO " + armor.EditorID + " carries keywords; this add-on's tokens " +
                           "must carry none (a box-token marker here joins the box pool)");
        }

        foreach (var error in errors) Console.Error.WriteLine("VERIFY FAIL: " + error);
        if (errors.Count != 0) return 1;
        Console.WriteLine("verify NPC: 16 ARMO + 16 ARMA, ESL, Skyrim.esm-only, links/slots/races/role-split OK");
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

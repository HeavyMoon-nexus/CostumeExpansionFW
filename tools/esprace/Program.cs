// esprace - add the community-standard playable-race list to every ArmorAddon
// in CostumeFW.esp.
//
// Why (2026-08-02, in-game proven on BD Ungulates deer race): every CEF token /
// carrier ARMA shipped as Race=DefaultRace with NO AdditionalRaces. The engine
// renders a worn addon only when the actor's race - or its RACE.ArmorRace
// proxy - appears in the ARMA's race set. Custom races whose ArmorRace points
// at a VANILLA race (BD Ungulates: ImperialRace) therefore never rendered the
// carrier: the token equips, the (invisible) carrier silently does not attach,
// FSMP gets no mesh, and every box content stays permanently static. Regular
// outfit mods dodge this by listing DefaultRace + all playables + vampires on
// each ARMA (see the reporter's MergeAudit: every one of 8824 ARMAs), which is
// exactly the list this tool applies.
//
// Idempotent: races already present are skipped. The ESP is backed up next to
// itself (.bak-<timestamp>) before writing.
//
// Usage:  dotnet run --project tools/esprace [-- <path-to-CostumeFW.esp>]

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Skyrim;

internal static class Program
{
    private const string kDefaultEsp =
        @"K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW\CostumeFW.esp";

    // The community-standard additional-race set (RNAM stays DefaultRace).
    // Taken verbatim from the wild (reporter's MergeAudit ARMA dumps): all ten
    // playable races, their vampire variants, Elder + ElderVampire, Manakin.
    private static readonly (uint Id, string Name)[] kRaces =
    {
        (0x013740, "ArgonianRace"),
        (0x013741, "BretonRace"),
        (0x013742, "DarkElfRace"),
        (0x013743, "HighElfRace"),
        (0x013744, "ImperialRace"),
        (0x013745, "KhajiitRace"),
        (0x013746, "NordRace"),
        (0x013747, "OrcRace"),
        (0x013748, "RedguardRace"),
        (0x013749, "WoodElfRace"),
        (0x067CD8, "ElderRace"),
        (0x088794, "NordRaceVampire"),
        (0x08883A, "ArgonianRaceVampire"),
        (0x08883C, "BretonRaceVampire"),
        (0x08883D, "DarkElfRaceVampire"),
        (0x088840, "HighElfRaceVampire"),
        (0x088844, "ImperialRaceVampire"),
        (0x088845, "KhajiitRaceVampire"),
        (0x088846, "RedguardRaceVampire"),
        (0x088884, "WoodElfRaceVampire"),
        (0x0A82B9, "OrcRaceVampire"),
        (0x0A82BA, "ElderRaceVampire"),
        (0x10760A, "ManakinRace"),
    };

    private static int Main(string[] args)
    {
        var espPath = args.Length > 0 ? args[0] : kDefaultEsp;
        try {
            return Run(espPath);
        } catch (Exception ex) {
            Console.Error.WriteLine("FATAL: " + ex);
            return 1;
        }
    }

    private static int Run(string espPath)
    {
        if (!File.Exists(espPath)) {
            Console.Error.WriteLine("missing esp: " + espPath);
            return 1;
        }

        var skyrim = ModKey.FromNameAndExtension("Skyrim.esm");
        var mod = SkyrimMod.CreateFromBinary(ModPath.FromPath(espPath), SkyrimRelease.SkyrimSE);

        int touched = 0, alreadyOk = 0, linksAdded = 0;
        foreach (var aa in mod.ArmorAddons) {
            var have = new HashSet<FormKey>(aa.AdditionalRaces.Select(r => r.FormKey));
            int before = have.Count;
            foreach (var (id, _) in kRaces) {
                var fk = new FormKey(skyrim, id);
                if (have.Add(fk)) {
                    aa.AdditionalRaces.Add(fk.ToLink<IRaceGetter>());
                    linksAdded++;
                }
            }
            if (have.Count != before) {
                touched++;
                Console.WriteLine($"  {aa.EditorID}: additional races {before} -> {have.Count}");
            } else {
                alreadyOk++;
            }
        }

        if (touched == 0) {
            Console.WriteLine($"nothing to do: all {alreadyOk} ArmorAddon(s) already carry the list");
            return 0;
        }

        var backup = espPath + ".bak-" + DateTime.Now.ToString("yyyyMMdd-HHmmss");
        File.Copy(espPath, backup);
        mod.WriteToBinary(espPath);

        Console.WriteLine();
        Console.WriteLine($"ArmorAddons touched: {touched} (already ok: {alreadyOk}), race links added: {linksAdded}");
        Console.WriteLine("backup: " + backup);
        Console.WriteLine("written: " + espPath);
        return 0;
    }
}

// nifcarrier — headless FSMP physics-carrier NIF authoring for CEF.
//
// Solves the shared B/C blocker (FSMP_APPROACH_B.md §9-4, FSMP_APPROACH_C.md §9-10):
// authoring a NIF that carries a physics bone hierarchy + the root
// NiStringExtraData "HDT Skinned Mesh Physics Object" (XML path), WITHOUT a
// NifSkope/Blender GUI. Uses ousnius/NiflySharp (GPL-3.0, nuget "Nifly").
//
// Commands:
//   dump       <nif>                 list blocks / bones / HDT extra data
//   carrier    <in.nif> <out.nif>    level-2 invisible carrier: strip all geometry
//                                    shapes, keep bone hierarchy + HDT extra data
//   verifytree <src>   <carrier>     assert bone parent hierarchy is identical
//
// A level-2 carrier is what you assign to a box token's ARMA WorldModel so FSMP's
// ①armor-attach path (doSkeletonMerge) builds hdtSSEPhysics_AutoRename_Armor_<id>
// bones with no visible double of the costume. CEF's suffix-match rebind
// (FindFsmpRenamedBone) then binds the injected mesh onto those bones.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using NiflySharp;
using NiflySharp.Blocks;

class Program
{
    const string HDT_EXTRA = "HDT Skinned Mesh Physics Object";

    static string ResolveString(object o)
    {
        if (o == null) return null;
        if (o is string s) return s;
        var t = o.GetType();
        if (t.GetProperty("String")?.GetValue(o) is string v1) return v1;
        if (t.GetField("String")?.GetValue(o) is string v2) return v2;
        return null;
    }

    static string NameOf(object block)
    {
        var t = block.GetType();
        object nameObj = t.GetProperty("Name")?.GetValue(block)
                         ?? t.GetField("Name")?.GetValue(block);
        return ResolveString(nameObj);
    }

    static void Report(NifFile nif, string tag)
    {
        var blocks = nif.Blocks.ToList();
        int shapes = nif.GetShapes()?.Count() ?? 0;
        var byType = new Dictionary<string, int>();
        int boneNodes = 0;
        bool hdt = false;
        foreach (var b in blocks)
        {
            string tn = b.GetType().Name;
            byType[tn] = byType.GetValueOrDefault(tn) + 1;
            if (tn == "NiNode" && NameOf(b) != null) boneNodes++;
            if (tn == "NiStringExtraData" && NameOf(b) == HDT_EXTRA) hdt = true;
        }
        Console.WriteLine($"--- {tag}: blocks={blocks.Count} shapes={shapes} namedNiNodes={boneNodes} hdtExtra={hdt}");
        foreach (var kv in byType.OrderBy(k => k.Key))
            Console.WriteLine($"      {kv.Value,3}  {kv.Key}");
    }

    static int MakeCarrier(string inPath, string outPath)
    {
        var nif = new NifFile();
        if (nif.Load(inPath, new NifFileLoadOptions()) != 0 || !nif.Valid)
        {
            Console.WriteLine($"[carrier] FAILED to load {inPath}");
            return 1;
        }
        Report(nif, "BEFORE");

        if (!nif.Blocks.Any(b => b.GetType().Name == "NiStringExtraData" && NameOf(b) == HDT_EXTRA))
            Console.WriteLine($"[carrier] WARNING: source has no '{HDT_EXTRA}' extra data — carrier will not drive physics.");

        // 1. Remove all geometry shapes (BSTriShape / BSDynamicTriShape / NiTriShape...).
        var shapes = nif.GetShapes().ToList();
        foreach (var s in shapes)
            nif.RemoveBlock((NiObject)s);
        Console.WriteLine($"[carrier] removed {shapes.Count} shape(s)");

        // 2. Sweep now-unreferenced skin/shader/geomdata/texture blocks. Bone NiNodes
        //    survive because they hang under the root NiNode child tree (reachable from
        //    root), which is exactly what FSMP's doSkeletonMerge walks.
        nif.RemoveUnreferencedBlocks();
        Report(nif, "AFTER");

        // 3. Save
        if (nif.Save(outPath, new NifFileSaveOptions()) != 0)
        {
            Console.WriteLine($"[carrier] FAILED to save {outPath}");
            return 1;
        }
        Console.WriteLine($"[carrier] wrote {outPath}");

        // 4. Reload and self-verify
        var chk = new NifFile();
        chk.Load(outPath, new NifFileLoadOptions());
        Report(chk, "RELOAD");
        int shp = chk.GetShapes()?.Count() ?? 0;
        bool hdt = chk.Blocks.Any(b => b.GetType().Name == "NiStringExtraData" && NameOf(b) == HDT_EXTRA);
        int bones = chk.Blocks.Count(b => b.GetType().Name == "NiNode" && NameOf(b) != null);
        Console.WriteLine($"[carrier] VERDICT shapes={shp} (want 0)  hdtExtra={hdt} (want True)  bones={bones} (want >0)");
        return (shp == 0 && hdt && bones > 0) ? 0 : 2;
    }

    static int Dump(string path)
    {
        var nif = new NifFile();
        nif.Load(path, new NifFileLoadOptions());
        Report(nif, "DUMP " + path);
        Console.WriteLine("named NiNodes:");
        foreach (var b in nif.Blocks.Where(x => x.GetType().Name == "NiNode"))
        {
            var nm = NameOf(b);
            if (nm != null) Console.WriteLine($"    {nm}");
        }
        return 0;
    }

    static Dictionary<string, string> ParentMap(NifFile nif)
    {
        var map = new Dictionary<string, string>();
        foreach (var b in nif.Blocks.Where(x => x.GetType().Name == "NiNode"))
        {
            string nm = NameOf(b);
            if (nm == null) continue;
            var parent = nif.GetParentNode((NiObject)b);
            map[nm] = parent != null ? (NameOf(parent) ?? "<root/unnamed>") : "<none>";
        }
        return map;
    }

    static int VerifyTree(string srcPath, string carrierPath)
    {
        var a = new NifFile(); a.Load(srcPath, new NifFileLoadOptions());
        var b = new NifFile(); b.Load(carrierPath, new NifFileLoadOptions());
        var ma = ParentMap(a);
        var mb = ParentMap(b);
        Console.WriteLine($"[verifytree] source bones={ma.Count}  carrier bones={mb.Count}");
        int mismatch = 0, missing = 0;
        foreach (var kv in ma)
        {
            if (!mb.TryGetValue(kv.Key, out var pb)) { missing++; Console.WriteLine($"    MISSING in carrier: {kv.Key}"); }
            else if (pb != kv.Value) { mismatch++; Console.WriteLine($"    PARENT DIFF: {kv.Key}: src='{kv.Value}' carrier='{pb}'"); }
        }
        Console.WriteLine($"[verifytree] VERDICT missing={missing} parentMismatch={mismatch} => {(missing == 0 && mismatch == 0 ? "IDENTICAL HIERARCHY" : "DIVERGED")}");
        return (missing == 0 && mismatch == 0) ? 0 : 2;
    }

    // Equivalent of NiflySharp's internal CloneNodesRec (public-API only): merge
    // srcNode's bone branch into dst, reusing an existing same-named bone or cloning
    // it under the name-matched parent (else dst root), then recursing children.
    // Same algorithm FSMP's doSkeletonMerge uses, so the union stays FSMP-consistent.
    static void MergeBranch(NifFile dst, NiNode srcNode, NiNode dstRoot, NifFile srcNif)
    {
        string boneName = srcNode.Name?.String;
        if (boneName == null) return;

        NiNode nodeParent = dstRoot;
        var srcParent = srcNif.GetParentNode(srcNode);
        if (srcParent?.Name?.String is string pn)
        {
            var p = dst.FindBlockByName<NiNode>(pn);
            if (p != null) nodeParent = p;
        }

        var existing = dst.FindBlockByName<NiNode>(boneName);
        if (!dst.GetBlockIndex(existing, out int boneID))
        {
            boneID = dst.CloneNamedNode(boneName, srcNif);
            nodeParent.Children.AddBlockRef(boneID);
        }

        foreach (var childRef in srcNode.Children.References.ToList())
        {
            var childNode = srcNif.GetBlock<NiNode>(childRef);
            if (childNode != null) MergeBranch(dst, childNode, dstRoot, srcNif);
        }
    }

    // Level-3 merge carrier: union the bone branches of many contents into one NIF.
    static int Merge(string outPath, string[] inputs)
    {
        if (inputs.Length < 2) { Console.WriteLine("[merge] need a base NIF + at least one more"); return 1; }
        var dst = new NifFile();
        if (dst.Load(inputs[0], new NifFileLoadOptions()) != 0 || !dst.Valid)
        { Console.WriteLine($"[merge] FAILED to load base {inputs[0]}"); return 1; }
        Report(dst, "BASE " + inputs[0]);
        var dstRoot = dst.GetRootNode();

        for (int i = 1; i < inputs.Length; i++)
        {
            var src = new NifFile();
            if (src.Load(inputs[i], new NifFileLoadOptions()) != 0 || !src.Valid)
            { Console.WriteLine($"[merge] FAILED to load {inputs[i]}"); return 1; }
            var srcRoot = src.GetRootNode();
            int before = dst.Blocks.Count(b => b.GetType().Name == "NiNode" && NameOf(b) != null);
            foreach (var childRef in srcRoot.Children.References.ToList())
            {
                var childNode = src.GetBlock<NiNode>(childRef);
                if (childNode != null) MergeBranch(dst, childNode, dstRoot, src);
            }
            int after = dst.Blocks.Count(b => b.GetType().Name == "NiNode" && NameOf(b) != null);
            Console.WriteLine($"[merge] {inputs[i]}: +{after - before} new bone(s) (shared bones reused)");
        }

        if (dst.Save(outPath, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[merge] FAILED to save {outPath}"); return 1; }
        var chk = new NifFile(); chk.Load(outPath, new NifFileLoadOptions());
        Report(chk, "MERGED " + outPath);
        bool hdt = chk.Blocks.Any(b => b.GetType().Name == "NiStringExtraData" && NameOf(b) == HDT_EXTRA);
        Console.WriteLine($"[merge] NOTE root extra data is inherited from base ('{inputs[0]}'); hdtExtra={hdt}.");
        Console.WriteLine("[merge]      The merged XML must union all contents' physics (see README / test doc).");
        return 0;
    }

    // Re-route: nest every root-level bone branch under a new anchor node named
    // e.g. "NPC Pelvis [Pelv]" so the physics chain grows from the body bone, not
    // the head, when driven via the facegen head path (C §9-10).
    static int Anchor(string inPath, string outPath, string anchorName)
    {
        var nif = new NifFile();
        if (nif.Load(inPath, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[anchor] FAILED to load {inPath}"); return 1; }
        Report(nif, "BEFORE");
        var root = nif.GetRootNode();

        var anchor = new NiNode { Name = new NiStringRef(anchorName) };
        int anchorId = nif.AddBlock(anchor);

        int moved = 0;
        foreach (var r in root.Children.References.ToList())
        {
            var n = nif.GetBlock<NiNode>(r);
            if (n != null && !ReferenceEquals(n, anchor))
            {
                anchor.Children.AddBlockRef(r.Index);
                r.Clear();
                moved++;
            }
        }
        root.Children.AddBlockRef(anchorId);
        Console.WriteLine($"[anchor] moved {moved} branch top(s) under new anchor '{anchorName}'");

        if (nif.Save(outPath, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[anchor] FAILED to save {outPath}"); return 1; }
        var chk = new NifFile(); chk.Load(outPath, new NifFileLoadOptions());
        Report(chk, "RELOAD");
        var a = chk.FindBlockByName<NiNode>(anchorName);
        int underAnchor = a?.Children.References.Count(r => chk.GetBlock<NiNode>(r) != null) ?? 0;
        Console.WriteLine($"[anchor] VERDICT anchor='{anchorName}' present={a != null} bonesUnderAnchor={underAnchor}");
        Console.WriteLine("[anchor] NOTE FMD path also needs a tiny skinned geometry on root (C §9-10 (b));");
        Console.WriteLine("[anchor]      not added here — anchor re-route only. See test doc.");
        return (a != null && underAnchor > 0) ? 0 : 2;
    }

    // Step-1 carrier: keep exactly ONE (smallest, skinned) geometry shape as the
    // engine attach trigger, remove the rest. Rationale: a zero-geometry carrier does
    // NOT fire FSMP — the engine never calls the biped-attach function (15535) for an
    // armor with no skinned geometry, so neither FSMP event fires and no bones get
    // merged (verified in-game 2026-07-02; mechanism per FSMP_REINVESTIGATION.md §2-1).
    // C §9-10 (b)'s "tiny skinned geometry" requirement applies to the ①armor path too.
    static int MakeCarrierKeep1(string inPath, string outPath)
    {
        var nif = new NifFile();
        if (nif.Load(inPath, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[keep1] FAILED to load {inPath}"); return 1; }
        Report(nif, "BEFORE");

        var shapes = nif.GetShapes().ToList();
        // Prefer plain skinned BSTriShape with the fewest vertices (smallest visible
        // footprint); BSDynamicTriShape (morph/facegen geometry) only as a last resort.
        var keep = shapes
            .Where(s => s.HasSkinInstance)
            .OrderBy(s => s.GetType().Name == "BSDynamicTriShape" ? 1 : 0)
            .ThenBy(s => (int)s.VertexCount)
            .FirstOrDefault();
        if (keep == null)
        { Console.WriteLine("[keep1] FAILED: no skinned shape in source"); return 1; }
        Console.WriteLine($"[keep1] retaining {keep.GetType().Name} '{NameOf(keep)}' verts={keep.VertexCount} as attach trigger");

        int removed = 0;
        foreach (var s in shapes)
            if (!ReferenceEquals(s, keep)) { nif.RemoveBlock((NiObject)s); removed++; }
        nif.RemoveUnreferencedBlocks();
        Console.WriteLine($"[keep1] removed {removed} shape(s), kept 1");
        Report(nif, "AFTER");

        if (nif.Save(outPath, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[keep1] FAILED to save {outPath}"); return 1; }
        Console.WriteLine($"[keep1] wrote {outPath}");

        var chk = new NifFile();
        chk.Load(outPath, new NifFileLoadOptions());
        Report(chk, "RELOAD");
        int shp = chk.GetShapes()?.Count() ?? 0;
        bool skinned = chk.GetShapes()?.Any(s => s.HasSkinInstance) ?? false;
        bool hdt = chk.Blocks.Any(b => b.GetType().Name == "NiStringExtraData" && NameOf(b) == HDT_EXTRA);
        int bones = chk.Blocks.Count(b => b.GetType().Name == "NiNode" && NameOf(b) != null);
        Console.WriteLine($"[keep1] VERDICT shapes={shp} (want 1)  skinned={skinned} (want True)  hdtExtra={hdt} (want True)  bones={bones} (want >0)");
        return (shp == 1 && skinned && hdt && bones > 0) ? 0 : 2;
    }

    // Zero every vertex field (Vertex / VertexHalf) in a boxed-struct list via
    // reflection. Works for BSVertexData, BSVertexDataSSE and any future variant.
    static int ZeroVertexList(System.Collections.IList list)
    {
        if (list == null || list.Count == 0) return 0;
        var itemType = list[0].GetType();
        var fields = new[] { itemType.GetField("Vertex"), itemType.GetField("VertexHalf") }
            .Where(f => f != null).ToArray();
        if (fields.Length == 0) return 0;
        for (int i = 0; i < list.Count; i++)
        {
            object boxed = list[i];
            foreach (var f in fields)
                f.SetValue(boxed, Activator.CreateInstance(f.FieldType));
            list[i] = boxed;
        }
        return list.Count;
    }

    // Step-2 invisibility: collapse all render vertices to the origin. The skinned
    // shape (and its skin instance) survives, so Skyrim's biped attach still returns
    // an attachedNode and FSMP still fires (keep1 requirement); the triangles are
    // degenerate, so nothing is drawn. For SSE skinned shapes the render vertex data
    // lives in NiSkinPartition (_vertexData), not in the BSTriShape itself — zero both.
    static int Collapse(string inPath, string outPath)
    {
        var nif = new NifFile();
        if (nif.Load(inPath, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[collapse] FAILED to load {inPath}"); return 1; }
        Report(nif, "BEFORE");

        int zeroed = 0;
        foreach (var shape in nif.GetShapes())
        {
            var t = shape.GetType();
            foreach (var propName in new[] { "VertexData", "VertexDataSSE" })
            {
                var p = t.GetProperty(propName);
                if (p?.GetValue(shape) is System.Collections.IList l)
                    zeroed += ZeroVertexList(l);
            }
        }
        foreach (var part in nif.Blocks.Where(b => b.GetType().Name == "NiSkinPartition"))
        {
            var f = part.GetType().GetField("_vertexData",
                BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Public);
            if (f?.GetValue(part) is System.Collections.IList l)
                zeroed += ZeroVertexList(l);
        }
        Console.WriteLine($"[collapse] zeroed {zeroed} vertex record(s)");
        if (zeroed == 0)
        { Console.WriteLine("[collapse] FAILED: found no vertex data to zero"); return 1; }

        if (nif.Save(outPath, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[collapse] FAILED to save {outPath}"); return 1; }
        Console.WriteLine($"[collapse] wrote {outPath}");

        // Reload and verify all partition/shape vertices are zero
        var chk = new NifFile();
        chk.Load(outPath, new NifFileLoadOptions());
        Report(chk, "RELOAD");
        int nonzero = 0, total = 0;
        foreach (var part in chk.Blocks.Where(b => b.GetType().Name == "NiSkinPartition"))
        {
            var f = part.GetType().GetField("_vertexData",
                BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Public);
            if (f?.GetValue(part) is System.Collections.IList l && l.Count > 0)
            {
                var vf = l[0].GetType().GetField("Vertex");
                for (int i = 0; i < l.Count; i++)
                {
                    total++;
                    if (vf != null && !vf.GetValue(l[i]).Equals(Activator.CreateInstance(vf.FieldType)))
                        nonzero++;
                }
            }
        }
        int shp = chk.GetShapes()?.Count() ?? 0;
        bool skinned = chk.GetShapes()?.Any(s => s.HasSkinInstance) ?? false;
        bool hdt = chk.Blocks.Any(b => b.GetType().Name == "NiStringExtraData" && NameOf(b) == HDT_EXTRA);
        Console.WriteLine($"[collapse] VERDICT partitionVerts={total} nonzero={nonzero} (want 0)  shapes={shp} skinned={skinned} hdtExtra={hdt}");
        return (nonzero == 0 && total > 0 && shp > 0 && skinned && hdt) ? 0 : 2;
    }

    // Step-2 invisibility, first choice (FSMP_REINVESTIGATION.md §3-6): the community
    // standard for invisible-but-equipped meshes is zero-alpha — NiAlphaProperty with
    // blending enabled + shader alpha = 0. The skinned shape still loads, attaches and
    // fires the armor path; it just draws fully transparent. (Vertex collapse remains
    // available as the second choice.)
    const int ALPHA_BLEND_FLAGS = 237; // blend enable | src=SRC_ALPHA | dst=INV_SRC_ALPHA

    static int ZeroAlpha(string inPath, string outPath)
    {
        var nif = new NifFile();
        if (nif.Load(inPath, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[zeroalpha] FAILED to load {inPath}"); return 1; }
        Report(nif, "BEFORE");

        int done = 0;
        foreach (var shape in nif.GetShapes().ToList()) // materialize: AddBlock below mutates Blocks
        {
            // 1. shader alpha -> 0 (BSLightingShaderProperty keeps it in private _alpha)
            var shader = nif.GetShader(shape);
            if (shader == null)
            { Console.WriteLine($"[zeroalpha] WARNING: shape '{NameOf(shape)}' has no shader, skipping"); continue; }
            var alphaField = shader.GetType().GetField("_alpha", BindingFlags.NonPublic | BindingFlags.Instance);
            if (alphaField == null)
            { Console.WriteLine($"[zeroalpha] WARNING: shader {shader.GetType().Name} has no _alpha field, skipping"); continue; }
            alphaField.SetValue(shader, 0f);

            // 2. ensure a NiAlphaProperty with blending enabled so alpha is honored
            NiAlphaProperty alphaProp = shape.HasAlphaProperty
                ? nif.GetBlock<NiAlphaProperty>(shape.AlphaPropertyRef)
                : null;
            if (alphaProp == null)
            {
                alphaProp = new NiAlphaProperty();
                int id = nif.AddBlock(alphaProp);
                shape.AlphaPropertyRef = new NiBlockRef<NiAlphaProperty>(id);
            }
            alphaProp.Flags = new NiflySharp.Bitfields.AlphaFlags((ushort)ALPHA_BLEND_FLAGS);
            alphaProp.Threshold = 0;
            Console.WriteLine($"[zeroalpha] shape '{NameOf(shape)}': shader alpha=0, NiAlphaProperty flags={ALPHA_BLEND_FLAGS}");
            done++;
        }
        if (done == 0)
        { Console.WriteLine("[zeroalpha] FAILED: no shape processed"); return 1; }

        if (nif.Save(outPath, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[zeroalpha] FAILED to save {outPath}"); return 1; }
        Console.WriteLine($"[zeroalpha] wrote {outPath}");

        // Reload and verify
        var chk = new NifFile();
        chk.Load(outPath, new NifFileLoadOptions());
        Report(chk, "RELOAD");
        bool allZero = true, allBlend = true;
        foreach (var shape in chk.GetShapes())
        {
            var shader = chk.GetShader(shape);
            var f = shader?.GetType().GetField("_alpha", BindingFlags.NonPublic | BindingFlags.Instance);
            if (f == null || (float)f.GetValue(shader) != 0f) allZero = false;
            var ap = shape.HasAlphaProperty ? chk.GetBlock<NiAlphaProperty>(shape.AlphaPropertyRef) : null;
            if (ap == null || ap.Flags.Value != ALPHA_BLEND_FLAGS) allBlend = false;
        }
        int shp = chk.GetShapes()?.Count() ?? 0;
        bool skinned = chk.GetShapes()?.Any(s => s.HasSkinInstance) ?? false;
        bool hdt = chk.Blocks.Any(b => b.GetType().Name == "NiStringExtraData" && NameOf(b) == HDT_EXTRA);
        Console.WriteLine($"[zeroalpha] VERDICT shaderAlphaZero={allZero} blendFlags={allBlend} shapes={shp} skinned={skinned} hdtExtra={hdt}");
        return (allZero && allBlend && shp > 0 && skinned && hdt) ? 0 : 2;
    }

    // T0 sanity: pure load -> save round-trip (no edits). Use the output in-game to
    // confirm the engine reads NiflySharp's writer at all, before trusting carriers.
    static int Passthrough(string inPath, string outPath)
    {
        var nif = new NifFile();
        if (nif.Load(inPath, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[passthrough] FAILED to load {inPath}"); return 1; }
        Report(nif, "IN");
        if (nif.Save(outPath, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[passthrough] FAILED to save {outPath}"); return 1; }
        var chk = new NifFile(); chk.Load(outPath, new NifFileLoadOptions());
        Report(chk, "OUT");
        Console.WriteLine("[passthrough] round-tripped. In-game test: does the engine render this NiflySharp output?");
        return 0;
    }

    static int Main(string[] args)
    {
        if (args.Length == 0)
        {
            Console.WriteLine("usage: nifcarrier dump       <nif>");
            Console.WriteLine("       nifcarrier carrier    <in.nif> <out.nif>");
            Console.WriteLine("       nifcarrier keep1      <in.nif> <out.nif>   (carrier keeping 1 skinned shape as attach trigger)");
            Console.WriteLine("       nifcarrier collapse   <in.nif> <out.nif>   (zero all render verts -> invisible; 2nd choice)");
            Console.WriteLine("       nifcarrier zeroalpha  <in.nif> <out.nif>   (shader alpha=0 + NiAlphaProperty blend -> invisible; 1st choice)");
            Console.WriteLine("       nifcarrier verifytree <src> <carrier>");
            Console.WriteLine("       nifcarrier merge      <out.nif> <base.nif> <add.nif> [add2.nif ...]");
            Console.WriteLine("       nifcarrier anchor     <in.nif> <out.nif> <anchorBoneName>");
            Console.WriteLine("       nifcarrier passthrough <in.nif> <out.nif>   (T0 sanity: load->save round-trip)");
            return 1;
        }
        try
        {
            switch (args[0])
            {
                case "dump": return Dump(args[1]);
                case "carrier": return MakeCarrier(args[1], args[2]);
                case "keep1": return MakeCarrierKeep1(args[1], args[2]);
                case "collapse": return Collapse(args[1], args[2]);
                case "zeroalpha": return ZeroAlpha(args[1], args[2]);
                case "verifytree": return VerifyTree(args[1], args[2]);
                case "merge": return Merge(args[1], args.Skip(2).ToArray());
                case "anchor": return Anchor(args[1], args[2], args[3]);
                case "passthrough": return Passthrough(args[1], args[2]);
                default: Console.WriteLine($"unknown command '{args[0]}'"); return 1;
            }
        }
        catch (IndexOutOfRangeException)
        {
            Console.WriteLine("error: missing argument(s) for command " + args[0]);
            return 1;
        }
    }
}

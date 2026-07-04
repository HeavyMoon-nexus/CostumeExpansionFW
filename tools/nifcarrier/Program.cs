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

    // Ensure a source bone (and its ancestor chain) exists in dst, cloned under the
    // same-named parent - or under dst root if the bone has NO node parent. This
    // rescues physics bones that are referenced ONLY by a shape's skin and are NOT in
    // the source's node hierarchy, which MergeBranch's Children-walk never reaches
    // (e.g. Pharaoh_Veil's PhSVeil_* bones). Idempotent: returns the existing dst node
    // if already present, so it never duplicates a shared skeleton bone.
    static NiNode EnsureBone(NifFile dst, NifFile src, NiNode srcBone, NiNode dstRoot)
    {
        string bn = srcBone.Name?.String;
        if (bn == null) return dstRoot;
        var existing = dst.FindBlockByName<NiNode>(bn);
        if (existing != null) return existing;
        NiNode dparent = dstRoot;
        var sp = src.GetParentNode(srcBone) as NiNode;
        if (sp != null && sp.Name?.String != null && !ReferenceEquals(sp, src.GetRootNode()))
            dparent = EnsureBone(dst, src, sp, dstRoot);  // parent first (chain-safe order)
        int bid = dst.CloneNamedNode(bn, src);
        dparent.Children.AddBlockRef(bid);
        return dst.FindBlockByName<NiNode>(bn) ?? dstRoot;
    }

    // Merge one source's bones into dst: the hierarchy-reachable branch, then the
    // union of EVERY remaining named bone (rescues skin-only physics bones like the
    // Pharaoh_Veil's PhSVeil_* that hang off a shape's skin but not the node tree).
    static int MergeBones(NifFile dst, NifFile src, NiNode dstRoot)
    {
        int before = dst.Blocks.Count(b => b.GetType().Name == "NiNode" && NameOf(b) != null);
        var srcRoot = src.GetRootNode();
        foreach (var childRef in srcRoot.Children.References.ToList())
        {
            var childNode = src.GetBlock<NiNode>(childRef);
            if (childNode != null) MergeBranch(dst, childNode, dstRoot, src);
        }
        foreach (var b in src.Blocks.Where(x => x.GetType().Name == "NiNode" && NameOf(x) != null).ToList())
            EnsureBone(dst, src, (NiNode)b, dstRoot);
        return dst.Blocks.Count(b => b.GetType().Name == "NiNode" && NameOf(b) != null) - before;
    }

    // True if nif saves to path AND reloads valid. Cross-file CloneShape can emit SSE
    // geometry that round-trips as null vertex data and throws on reload - the gate
    // used to keep a corrupting content bones-only WITHOUT losing others' shapes.
    static bool SaveReloadOk(NifFile nif, string path)
    {
        try
        {
            if (nif.Save(path, new NifFileSaveOptions()) != 0) return false;
            var chk = new NifFile();
            return chk.Load(path, new NifFileLoadOptions()) == 0 && chk.Valid;
        }
        catch { return false; }
    }

    static void TryDelete(string p) { try { System.IO.File.Delete(p); } catch { } }

    // Level-3 merge carrier: union the bone branches of many contents into one NIF.
    // Bones ALWAYS union (FSMP builds every content's physics from bones + XML). Shapes
    // (collision meshes the XML may reference by name) are added PER CONTENT and kept
    // only if the NIF still reloads - so a content whose SSE geometry NiflySharp can't
    // clone cleanly (e.g. Pharaoh_Veil) is kept bones-only WITHOUT stripping the OTHER
    // contents' collision shapes.
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

            int addedBones = MergeBones(dst, src, dstRoot);  // bones ALWAYS union

            // Clone this content's shapes, remembering the new ones so we can drop JUST
            // them if they corrupt the NIF - keeping every OTHER content's collision mesh.
            var addedShapes = new List<NiObject>();
            foreach (var srcShape in src.GetShapes().ToList())
            {
                string nm = NameOf(srcShape) ?? "";
                if (dst.GetShapes().Any(s => (NameOf(s) ?? "") == nm))
                { Console.WriteLine($"[merge] WARNING: duplicate shape name '{nm}' from {inputs[i]} — first wins"); continue; }
                dst.CloneShape(srcShape, nm, src);
                var ns = dst.GetShapes().FirstOrDefault(s => (NameOf(s) ?? "") == nm);
                if (ns != null) addedShapes.Add((NiObject)ns);
            }
            dst.RemoveUnreferencedBlocks();

            // Validate to a throwaway temp (unique per content, so a lingering NiflySharp
            // read handle can't collide with the next write). Unloadable -> remove just
            // this content's shapes in memory (its bones stay) = bones-only for it.
            string vtmp = outPath + $".v{i}";
            bool ok = SaveReloadOk(dst, vtmp);
            TryDelete(vtmp);
            if (ok)
            {
                Console.WriteLine($"[merge] {inputs[i]}: +{addedBones} bone(s), +{addedShapes.Count} shape(s)");
            }
            else
            {
                foreach (var s in addedShapes) dst.RemoveBlock(s);
                dst.RemoveUnreferencedBlocks();
                Console.WriteLine($"[merge] {inputs[i]}: +{addedBones} bone(s), shapes unloadable (NiflySharp SSE clone) — kept BONES-ONLY");
            }
        }

        if (dst.Save(outPath, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[merge] FAILED to save {outPath}"); return 1; }
        var chk = new NifFile();
        try
        {
            if (chk.Load(outPath, new NifFileLoadOptions()) != 0 || !chk.Valid)
            { Console.WriteLine("[merge] merged NIF invalid on reload"); return 2; }
        }
        catch (Exception e) { Console.WriteLine($"[merge] merged NIF failed to reload: {e.Message}"); return 2; }
        Report(chk, "MERGED " + outPath);
        bool hdt = chk.Blocks.Any(b => b.GetType().Name == "NiStringExtraData" && NameOf(b) == HDT_EXTRA);
        Console.WriteLine($"[merge] hdtExtra={hdt} (root extra inherited from base '{inputs[0]}').");
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
            {
                // No shader = the engine never renders it (collision-proxy shapes like
                // 'VirtualHead'). Already invisible; nothing to do.
                Console.WriteLine($"[zeroalpha] shape '{NameOf(shape)}': no shader (non-rendered collision proxy), left as-is");
                done++;
                continue;
            }
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
            if (shader == null) continue; // non-rendered collision proxy — invisible by construction
            var f = shader.GetType().GetField("_alpha", BindingFlags.NonPublic | BindingFlags.Instance);
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

    // Merge several HDT-SMP physics XMLs into one <system> document (T2 unified XML).
    //
    // FSMP parser semantics that shape this (hdtSkyrimSystem.cpp, verified on dev):
    // - Anonymous <bone-default> overwrites the "" template used by every following
    //   <bone> without a template= attribute — concatenation would leak doc1's last
    //   default into doc2's bones.
    // - getBoneTemplate(unknownName) falls back to the CURRENT "" template, so a
    //   factory reset is expressed by snapshotting the pristine "" template at the top
    //   (<bone-default name="cef_factory"/>) and re-storing it at each document
    //   boundary (<bone-default extends="cef_factory"/>). Same for the three
    //   constraint-default kinds.
    // - Duplicate <bone name> entries are skipped by the parser ("already exists");
    //   we drop them ourselves too so shared anchors (NPC Pelvis...) merge cleanly.
    static readonly string[] DEFAULT_KINDS = {
        "bone-default", "generic-constraint-default",
        "stiffspring-constraint-default", "conetwist-constraint-default"
    };

    static int MergeXml(string outPath, string[] inputs)
    {
        if (inputs.Length < 2) { Console.WriteLine("[mergexml] need at least 2 input xml files"); return 1; }
        var docs = inputs.Select(System.Xml.Linq.XDocument.Load).ToList();
        foreach (var (d, i) in docs.Select((d, i) => (d, i)))
            if (d.Root?.Name.LocalName != "system")
            { Console.WriteLine($"[mergexml] FAILED: root of {inputs[i]} is not <system>"); return 1; }

        var outRoot = new System.Xml.Linq.XElement(docs[0].Root.Name, docs[0].Root.Attributes());
        foreach (var kind in DEFAULT_KINDS)
            outRoot.Add(new System.Xml.Linq.XElement(kind, new System.Xml.Linq.XAttribute("name", "cef_factory")));

        var seenBones = new HashSet<string>();
        var seenShapes = new HashSet<string>();
        int dupBones = 0;
        for (int i = 0; i < docs.Count; i++)
        {
            if (i > 0)
                foreach (var kind in DEFAULT_KINDS)
                    outRoot.Add(new System.Xml.Linq.XElement(kind, new System.Xml.Linq.XAttribute("extends", "cef_factory")));
            foreach (var el in docs[i].Root.Elements())
            {
                string tag = el.Name.LocalName;
                string nm = el.Attribute("name")?.Value;
                if (tag == "bone" && nm != null)
                {
                    if (!seenBones.Add(nm))
                    {
                        dupBones++;
                        Console.WriteLine($"[mergexml] duplicate bone '{nm}' from {inputs[i]} skipped (first wins)");
                        continue;
                    }
                }
                if ((tag == "per-vertex-shape" || tag == "per-triangle-shape") && nm != null && !seenShapes.Add(nm))
                    Console.WriteLine($"[mergexml] WARNING: duplicate collision shape '{nm}' from {inputs[i]} — NIF shape names must be unique across merged contents");
                outRoot.Add(new System.Xml.Linq.XElement(el));
            }
            Console.WriteLine($"[mergexml] appended {inputs[i]}");
        }

        var outDoc = new System.Xml.Linq.XDocument(new System.Xml.Linq.XDeclaration("1.0", "UTF-8", null), outRoot);
        outDoc.Save(outPath);
        Console.WriteLine($"[mergexml] wrote {outPath}  (bones={seenBones.Count} dupSkipped={dupBones} shapes={seenShapes.Count})");
        return 0;
    }

    // Re-point the carrier's root NiStringExtraData "HDT Skinned Mesh Physics Object"
    // at a different physics XML (e.g. the merged one from mergexml).
    static int SetXml(string inPath, string outPath, string xmlPath)
    {
        var nif = new NifFile();
        if (nif.Load(inPath, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[setxml] FAILED to load {inPath}"); return 1; }

        var extra = nif.Blocks.OfType<NiStringExtraData>()
            .FirstOrDefault(b => b.Name?.String == HDT_EXTRA);
        if (extra == null)
        { Console.WriteLine($"[setxml] FAILED: no '{HDT_EXTRA}' NiStringExtraData in {inPath}"); return 1; }
        string old = extra.StringData?.String;
        extra.StringData = new NiStringRef(xmlPath);
        Console.WriteLine($"[setxml] '{old}' -> '{xmlPath}'");

        if (nif.Save(outPath, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[setxml] FAILED to save {outPath}"); return 1; }

        var chk = new NifFile();
        chk.Load(outPath, new NifFileLoadOptions());
        var back = chk.Blocks.OfType<NiStringExtraData>().FirstOrDefault(b => b.Name?.String == HDT_EXTRA)?.StringData?.String;
        Console.WriteLine($"[setxml] VERDICT readback='{back}' (want '{xmlPath}')");
        return back == xmlPath ? 0 : 2;
    }

    // ---------------------------------------------------------------------------
    // sync — production driver (approach B, merged carriers). Reads the manifest
    // CEF writes alongside its settings and (re)builds, per box:
    //   0 SMP contents  -> copy of the empty token NIF (keeps per-token ARMAs valid)
    //   1 SMP content   -> zeroalpha carrier, content's own physics XML untouched
    //   2+ SMP contents -> zeroalpha(base w/ per-shape collision) + merge bone
    //                      branches + mergexml + setxml -> Box<slot>_physics.xml
    // A content is "SMP" iff its resolved NIF carries the HDT extra data. Boxes
    // whose inputs are unchanged (hash file) are skipped.
    //
    // Manifest (written by CEF, Data\SKSE\Plugins\CEF_carrier_manifest.json):
    // { "version":1, "boxes":[ { "slot":44, "token":"000801:CostumeFW_Boxes.esp",
    //     "contents":[ { "id":"000C5B:....esp", "nif":"Caenarvon\\...\\X.nif" } ] } ] }
    // "nif" is the ARMA model path (meshes-relative), resolved against --data roots.
    static string ResolveAgainstRoots(string rel, List<string> roots, bool meshesRel)
    {
        foreach (var r in roots)
        {
            var p1 = System.IO.Path.Combine(r, meshesRel ? "meshes" : "", rel.Replace('/', '\\'));
            if (System.IO.File.Exists(p1)) return p1;
            var p2 = System.IO.Path.Combine(r, rel.Replace('/', '\\'));
            if (System.IO.File.Exists(p2)) return p2;
        }
        return null;
    }

    static string GetHdtXmlPath(string nifPath)
    {
        var nif = new NifFile();
        if (nif.Load(nifPath, new NifFileLoadOptions()) != 0 || !nif.Valid) return null;
        return nif.Blocks.OfType<NiStringExtraData>()
            .FirstOrDefault(b => b.Name?.String == HDT_EXTRA)?.StringData?.String;
    }

    // Geometry block types that must NOT appear in an SSE carrier. SSE skinned
    // meshes are BSTriShape/BSDynamicTriShape; a skinned NiTriShape (LE/Oldrim, or
    // a badly-converted mesh) carrying an SSE NiSkinPartition is the classic
    // EXCEPTION_INT_DIVIDE_BY_ZERO in the engine's skin-partition loader
    // (root cause of the Box44 CTD, 2026-07-03).
    static readonly string[] NON_SSE_GEO = {
        "NiTriShape", "NiTriShapeData", "NiTriStrips", "NiTriStripsData"
    };

    // Reject a NIF the SSE engine would divide-by-zero on while building a skin
    // partition. Returns false + a human-readable reason on the FIRST problem, so
    // a poisoned content never reaches a carrier that a worn box token loads.
    static bool ValidateNifSkinnable(NifFile nif, out string reason)
    {
        // 1. Non-SSE (LE) geometry. Must be SSE-optimized (BSTriShape) first.
        foreach (var b in nif.Blocks)
        {
            string tn = b.GetType().Name;
            if (Array.IndexOf(NON_SSE_GEO, tn) >= 0)
            {
                reason = $"non-SSE geometry block {tn} (run SSE NIF Optimizer on the source)";
                return false;
            }
        }
        // 2. Skinned shape with zero vertices -> degenerate/empty partition, a
        //    prime divide-by-zero trigger in the partition loader.
        foreach (var s in nif.GetShapes())
        {
            if (s.HasSkinInstance && (int)s.VertexCount == 0)
            {
                reason = $"skinned shape '{NameOf(s)}' has 0 vertices";
                return false;
            }
        }
        reason = null;
        return true;
    }

    // Load a NIF and validate it in one step (false on unreadable OR invalid).
    static bool ValidateNifPath(string path, out string reason)
    {
        var nif = new NifFile();
        if (nif.Load(path, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { reason = "unreadable / invalid NIF"; return false; }
        return ValidateNifSkinnable(nif, out reason);
    }

    // Standalone diagnostic: is this NIF safe to bake into an SSE carrier, or would
    // the engine's skin-partition loader divide-by-zero on it? Use it to vet a
    // content NIF before adding it to a box.
    static int Validate(string path)
    {
        var nif = new NifFile();
        if (nif.Load(path, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[validate] {path}: FAILED to load (unreadable / invalid NIF)"); return 2; }
        Report(nif, "VALIDATE " + path);
        bool ok = ValidateNifSkinnable(nif, out string reason);
        Console.WriteLine(ok
            ? "[validate] VERDICT: OK (no divide-by-zero risk found)"
            : $"[validate] VERDICT: REJECT — {reason}");
        return ok ? 0 : 2;
    }

    // ---------------------------------------------------------------------------
    // approach-C head carrier (facegen path ②). Structurally a box carrier
    // (physics bones + zero-alpha'd shapes + HDT xml extra data), plus three
    // requirements the facegen path adds on top of the armor path:
    //   R1. the "HDT Skinned Mesh Physics Object" extra data must hang on the
    //       MODEL ROOT node - FSMP's scanBBP walks only the root's own extra
    //       data list, and the head path scans the head-part model root
    //       (FMD->faceNode; APPROACH_C §9-2).
    //   R2. at least one skinned shape must list, in its skin bones, a bone
    //       that is NOT in the live skeleton - the head path calls
    //       doSkeletonMerge only from the per-bone unresolved branch
    //       (ActorManager.cpp:1535); a carrier skinned purely to vanilla bones
    //       never merges its physics chains and physics stays dead
    //       (FSMP_REINVESTIGATION.md §2-2). The armor path has no such gate.
    //   R3. every custom bone must be REACHABLE in the node tree -
    //       doSkeletonMerge walks the scene graph only, so a bone block that
    //       exists but hangs off nothing (skin-only, e.g. Pharaoh_Veil's
    //       PhSVeil_*) is never created on the skeleton (the head-path twin of
    //       the §3-C merge bug; multi-content Merge already unions these,
    //       single-content carriers need the tree completed explicitly).
    //
    // "Not in the live skeleton" can't be known offline; the proxy: vanilla /
    // XPMSSE skeleton nodes follow well-known naming, custom physics bones
    // don't. In-game `cef headdiag` remains the ground truth.
    static readonly string[] LIVE_BONE_PREFIXES = {
        "NPC ", "CME ", "MOV ", "AnimObject", "Camera", "Weapon", "Shield",
        "Quiver", "SaddleBone", "HorseSpine", "Bip01"
    };
    static readonly string[] LIVE_BONE_EXACT = {
        "NPC", "Root", "MagicEffectsNode", "BSFaceGenNiNodeSkinned"
    };

    static bool LooksLiveSkeletonBone(string name)
    {
        if (string.IsNullOrEmpty(name)) return true;
        foreach (var p in LIVE_BONE_PREFIXES)
            if (name.StartsWith(p, StringComparison.OrdinalIgnoreCase)) return true;
        foreach (var e in LIVE_BONE_EXACT)
            if (string.Equals(name, e, StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    // The root-attached HDT extra data (or null): R1 above.
    static NiStringExtraData RootHdtExtra(NifFile nif)
    {
        var root = nif.GetRootNode();
        if (root == null) return null;
        foreach (var r in root.ExtraDataList.References)
        {
            var sed = nif.GetBlock<NiStringExtraData>(r);
            if (sed != null && sed.Name?.String == HDT_EXTRA) return sed;
        }
        return null;
    }

    // Re-attach a stray HDT extra data to the root's extra-data list (R1
    // fix-up). SMP convention IS root, but a content that carries it elsewhere
    // would work on the armor path and silently die on the head path.
    static bool EnsureRootHdtExtra(NifFile nif)
    {
        if (RootHdtExtra(nif) != null) return true;
        var stray = nif.Blocks.OfType<NiStringExtraData>()
            .FirstOrDefault(b => b.Name?.String == HDT_EXTRA);
        if (stray == null) return false;
        if (!nif.GetBlockIndex(stray, out int id)) return false;
        nif.GetRootNode().ExtraDataList.AddBlockRef(id);
        Console.WriteLine("[headcarrier] HDT extra data was not root-attached - re-attached to root");
        return RootHdtExtra(nif) != null;
    }

    // All node names reachable from the root via Children (the tree
    // doSkeletonMerge actually walks). Root's own name included.
    static HashSet<string> TreeNodeNames(NifFile nif)
    {
        var names = new HashSet<string>();
        void Walk(NiNode n)
        {
            if (n == null) return;
            var nm = n.Name?.String;
            if (nm != null && !names.Add(nm)) return;  // cycle/dup guard
            foreach (var r in n.Children.References)
                if (nif.GetBlock<NiNode>(r) is NiNode c) Walk(c);
        }
        Walk(nif.GetRootNode());
        return names;
    }

    // R3 fix-up: parent every orphaned named NiNode block (referenced only from
    // a skin instance's bone ptr array, not from the scene graph) under the
    // root, so the head path's doSkeletonMerge can see it. Same rescue
    // MergeBones does across files, applied within one file.
    static int CompleteBoneTree(NifFile nif)
    {
        var root = nif.GetRootNode();
        int reparented = 0;
        foreach (var b in nif.Blocks.Where(x => x.GetType().Name == "NiNode" && NameOf(x) != null).ToList())
        {
            var node = (NiNode)b;
            if (ReferenceEquals(node, root)) continue;
            if (nif.GetParentNode(node) != null) continue;
            if (!nif.GetBlockIndex(node, out int id)) continue;
            root.Children.AddBlockRef(id);
            reparented++;
            Console.WriteLine($"[headcarrier] orphan bone '{NameOf(node)}' re-parented under root (was skin-only)");
        }
        return reparented;
    }

    // Anchor re-route that (unlike the plain `anchor` verb) reuses an existing
    // same-named node and leaves live-named branches (NPC *, CME *, ...) where
    // they are - so a content whose chains already hang under "NPC Pelvis
    // [Pelv]" is not double-nested (APPROACH_C §9-10 recipe).
    static (int moved, int kept) AnchorInMemory(NifFile nif, string anchorName)
    {
        var root = nif.GetRootNode();
        var anchor = nif.FindBlockByName<NiNode>(anchorName);
        if (anchor == null)
        {
            anchor = new NiNode { Name = new NiStringRef(anchorName) };
            int anchorId = nif.AddBlock(anchor);
            root.Children.AddBlockRef(anchorId);
        }
        int moved = 0, kept = 0;
        foreach (var r in root.Children.References.ToList())
        {
            var n = nif.GetBlock<NiNode>(r);
            if (n == null || ReferenceEquals(n, anchor)) continue;
            string nm = NameOf(n);
            if (nm != null && LooksLiveSkeletonBone(nm)) { kept++; continue; }
            anchor.Children.AddBlockRef(r.Index);
            r.Clear();
            moved++;
        }
        return (moved, kept);
    }

    static int AnchorSmart(string inPath, string outPath, string anchorName)
    {
        var nif = new NifFile();
        if (nif.Load(inPath, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[anchorsmart] FAILED to load {inPath}"); return 1; }
        var (moved, kept) = AnchorInMemory(nif, anchorName);
        Console.WriteLine($"[anchorsmart] '{anchorName}': moved {moved} root branch(es) under it, kept {kept} live-named branch(es) at root");
        if (nif.Save(outPath, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[anchorsmart] FAILED to save {outPath}"); return 1; }
        return 0;
    }

    // Offline gate for a facegen-path carrier: R1/R2/R3 above plus the SSE
    // divide-by-zero gate. Also useful on a known-good SMP hair NIF to
    // sanity-check the heuristics (expected: OK), or on a raw content to see
    // whether it needs headcarrier's fix-ups (veil-likes fail R3).
    static int ValidateHead(string path)
    {
        var nif = new NifFile();
        if (nif.Load(path, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[validatehead] {path}: FAILED to load (unreadable / invalid NIF)"); return 2; }
        Report(nif, "VALIDATEHEAD " + path);

        bool ctdOk = ValidateNifSkinnable(nif, out string ctdReason);
        var rootExtra = RootHdtExtra(nif);
        bool anyExtra = nif.Blocks.OfType<NiStringExtraData>().Any(b => b.Name?.String == HDT_EXTRA);
        int bones = nif.Blocks.Count(b => b.GetType().Name == "NiNode" && NameOf(b) != null);
        var tree = TreeNodeNames(nif);

        int triggers = 0, shaderless = 0;
        var unreachable = new HashSet<string>();
        foreach (var s in nif.GetShapes())
        {
            // R4: the facegen pipeline breaks on shader-less geometry (collision
            // proxies like VirtualHead/VirtualGround). The ARMOR path tolerates
            // them (proven in box carriers); as a HEAD PART geometry they took the
            // whole actor 3D down (v2 PoC, 2026-07-04: character invisible).
            bool hasShader = nif.GetShader(s) != null;
            if (!hasShader) shaderless++;
            if (!s.HasSkinInstance)
            { Console.WriteLine($"    shape '{NameOf(s)}': UNSKINNED (no merge trigger){(hasShader ? "" : "  NO-SHADER (facegen-fatal)")}"); continue; }
            var names = nif.GetShapeBoneNames(s);
            var custom = names.Where(n => !LooksLiveSkeletonBone(n)).ToList();
            bool trig = custom.Count > 0;
            if (trig) triggers++;
            foreach (var c in custom)
                if (!tree.Contains(c)) unreachable.Add(c);
            Console.WriteLine($"    shape '{NameOf(s)}' verts={(int)s.VertexCount}: skinBones={names.Count} custom={custom.Count}{(trig ? "  TRIGGER" : "  (vanilla-only)")}{(hasShader ? "" : "  NO-SHADER (facegen-fatal)")}");
        }

        // §9-10 anchor report: doSkeletonMerge resolves live-named nodes GLOBALLY
        // and attaches each custom chain under its nearest NAMED ancestor - so the
        // effective merge anchor of every chain is knowable offline. A chain top
        // whose anchor is the model root attaches under the npc root node (works,
        // but the rest pose then ignores any intended body-bone frame).
        var rootNode = nif.GetRootNode();
        var anchors = new Dictionary<string, string>();  // chainTop -> anchor
        foreach (var b in nif.Blocks.Where(x => x.GetType().Name == "NiNode" && NameOf(x) != null))
        {
            if (ReferenceEquals(b, rootNode)) continue;  // the model root is not a bone
            string nm = NameOf(b);
            if (LooksLiveSkeletonBone(nm)) continue;
            // nearest NAMED ancestor (unnamed nodes are transparent to the merge)
            var p = nif.GetParentNode((NiObject)b);
            while (p != null && p.Name?.String == null && !ReferenceEquals(p, rootNode))
                p = nif.GetParentNode(p);
            string pn = p == null ? "(orphan)" : (ReferenceEquals(p, rootNode) ? "(model root)" : p.Name?.String ?? "(model root)");
            if (pn != "(orphan)" && pn != "(model root)" && !LooksLiveSkeletonBone(pn))
                continue;  // parent is custom too -> not a chain top
            anchors[nm] = pn;
        }
        foreach (var kv in anchors.OrderBy(k => k.Value).ThenBy(k => k.Key))
            Console.WriteLine($"    chain top '{kv.Key}' -> anchors under '{kv.Value}'");

        Console.WriteLine($"[validatehead] R1 rootHdtExtra={(rootExtra != null)}"
            + (rootExtra == null && anyExtra ? "  (extra data EXISTS but NOT on root - scanBBP misses it)" : "")
            + (rootExtra != null ? $"  xml='{rootExtra.StringData?.String}'" : ""));
        Console.WriteLine($"[validatehead] R2 mergeTriggerShapes={triggers} (skinned shape referencing a non-live bone)");
        Console.WriteLine($"[validatehead] R3 unreachableCustomBones={unreachable.Count}"
            + (unreachable.Count > 0 ? $"  ({string.Join(", ", unreachable.Take(5))}{(unreachable.Count > 5 ? ", ..." : "")}) - doSkeletonMerge would never create these" : ""));
        Console.WriteLine($"[validatehead] R4 shaderlessShapes={shaderless}"
            + (shaderless > 0 ? "  — facegen-fatal: the head build breaks on shader-less geometry (use headcarrier --keep1)" : ""));
        Console.WriteLine($"[validatehead] bones={bones}  sseSkinnable={ctdOk}{(ctdOk ? "" : " — " + ctdReason)}");
        bool pass = ctdOk && rootExtra != null && bones > 0 && triggers > 0 && unreachable.Count == 0 && shaderless == 0;
        Console.WriteLine($"[validatehead] VERDICT: {(pass ? "OK (should fire the facegen path)" : "REJECT")}");
        return pass ? 0 : 2;
    }

    // One-shot facegen-path carrier build:
    //   base pick (a content owning a merge-trigger shape; its shapes load
    //   in-place and never cross-file CloneShape, so the trigger survives even
    //   when another content falls back to bones-only) -> merge (2+ contents)
    //   -> zero-alpha -> in-memory final pass (R3 bone-tree completion,
    //   optional anchor re-route, optional xml re-point, R1 root fix-up)
    //   -> validatehead gate.
    // NOTE multi-content: pass --xml with a PRE-MERGED xml (see mergexml); this
    // verb takes disk paths and cannot resolve the contents' game-relative xml
    // paths. Per-content anchors: run anchorsmart on each content first.
    static int HeadCarrier(string[] args)
    {
        string outPath = null, xmlRel = null, anchorName = null;
        bool keep1 = false, proxyShader = false;
        var contents = new List<string>();
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--xml": xmlRel = args[++i]; break;
                case "--anchor": anchorName = args[++i]; break;
                case "--keep1": keep1 = true; break;
                case "--proxyshader": proxyShader = true; break;
                default:
                    if (outPath == null) outPath = args[i];
                    else contents.Add(args[i]);
                    break;
            }
        }
        if (outPath == null || contents.Count == 0)
        { Console.WriteLine("usage: headcarrier <out.nif> <content.nif> [content2 ...] [--xml <gameRelXml>] [--anchor <liveBoneName>] [--keep1] [--proxyshader]"); return 1; }

        // CTD gate per content - loud fail (exclusion policy lives in sync, not here)
        foreach (var c in contents)
            if (!ValidateNifPath(c, out string why))
            { Console.WriteLine($"[headcarrier] REJECT content '{c}' — {why}"); return 1; }

        // R2: base = first content owning a merge-trigger shape
        int baseIdx = -1;
        for (int i = 0; i < contents.Count && baseIdx < 0; i++)
        {
            var n = new NifFile();
            if (n.Load(contents[i], new NifFileLoadOptions()) != 0 || !n.Valid) continue;
            foreach (var s in n.GetShapes())
                if (s.HasSkinInstance && n.GetShapeBoneNames(s).Any(b => !LooksLiveSkeletonBone(b)))
                { baseIdx = i; break; }
        }
        if (baseIdx < 0)
        {
            Console.WriteLine("[headcarrier] FAILED: no content has a skinned shape referencing a custom bone —");
            Console.WriteLine("[headcarrier]         the facegen path would never call doSkeletonMerge (R2, FSMP_REINVESTIGATION.md §2-2)");
            return 1;
        }
        if (baseIdx != 0)
            Console.WriteLine($"[headcarrier] base reordered to '{contents[baseIdx]}' (owns a merge-trigger shape)");
        var ordered = new List<string> { contents[baseIdx] };
        ordered.AddRange(contents.Where((_, i) => i != baseIdx));

        string tMerged = outPath + ".hc1.nif";
        string tZa = outPath + ".hc2.nif";
        try
        {
            string cur = ordered[0];
            if (ordered.Count > 1)
            {
                if (Merge(tMerged, ordered.ToArray()) != 0) return 1;
                if (xmlRel == null)
                    Console.WriteLine("[headcarrier] WARNING: 2+ contents but no --xml — carrier keeps the base's xml; the other contents' physics configs are NOT merged");
                cur = tMerged;
            }
            if (ZeroAlpha(cur, tZa) != 0) return 1;

            // Final pass (in-memory): R3 completion -> anchor -> xml -> R1 -> save
            var fin = new NifFile();
            if (fin.Load(tZa, new NifFileLoadOptions()) != 0 || !fin.Valid)
            { Console.WriteLine("[headcarrier] FAILED to reload assembled carrier"); return 1; }
            int orphans = CompleteBoneTree(fin);
            if (orphans > 0) Console.WriteLine($"[headcarrier] R3: re-parented {orphans} skin-only bone(s) into the tree");
            if (keep1)
            {
                // Facegen-safe minimum: keep exactly ONE shape - the smallest
                // shader-bearing skinned shape referencing a custom bone (the
                // merge trigger). Everything else - including shader-less
                // collision proxies (VirtualHead...), which the facegen head
                // build cannot survive (R4) - is dropped. XML collision refs to
                // dropped shapes go unresolved; FSMP warns and disables that
                // collision. Runs AFTER CompleteBoneTree so skin-only bones are
                // already tree-parented and survive the unreferenced-block sweep.
                var all = fin.GetShapes().ToList();
                var keep = all
                    .Where(s => s.HasSkinInstance && fin.GetShader(s) != null
                        && fin.GetShapeBoneNames(s).Any(b => !LooksLiveSkeletonBone(b)))
                    .OrderBy(s => (int)s.VertexCount)
                    .FirstOrDefault();
                if (keep == null)
                { Console.WriteLine("[headcarrier] FAILED: --keep1 found no shader-bearing trigger shape"); return 1; }
                var kept = new List<NiflySharp.INiShape> { keep };
                if (proxyShader)
                {
                    // Keep shader-less SKINNED collision proxies (VirtualHead...) by
                    // pointing them at the kept shape's shader block - zero-alpha'd,
                    // so still invisible, but present: R4 (facegen breaks on
                    // shader-less geometry) is satisfied AND the physics XML's
                    // per-*-shape collision meshes stay resolvable in the facegen
                    // system (dropping VirtualHead cost the veil its head collision,
                    // in-game 2026-07-04).
                    int shaderId = -1;
                    var keptRefObj = keep.GetType().GetProperty("ShaderPropertyRef")?.GetValue(keep);
                    if (keptRefObj != null)
                        shaderId = (int)(keptRefObj.GetType().GetProperty("Index")?.GetValue(keptRefObj) ?? -1);
                    foreach (var s in all)
                    {
                        if (ReferenceEquals(s, keep) || !s.HasSkinInstance) continue;
                        if (fin.GetShader(s) != null) continue;  // shader-bearing extras still drop (minimal carrier)
                        var prop = s.GetType().GetProperty("ShaderPropertyRef");
                        if (shaderId >= 0 && prop != null && prop.CanWrite)
                        {
                            prop.SetValue(s, Activator.CreateInstance(prop.PropertyType, shaderId));
                            kept.Add(s);
                            Console.WriteLine($"[headcarrier] proxyshader: kept collision proxy '{NameOf(s)}' sharing '{NameOf(keep)}' shader (zero-alpha)");
                        }
                        else
                        {
                            Console.WriteLine($"[headcarrier] proxyshader: WARNING cannot set shader on '{NameOf(s)}' ({s.GetType().Name}) - dropped");
                        }
                    }
                }
                int dropped = 0;
                foreach (var s in all)
                    if (!kept.Contains(s)) { fin.RemoveBlock((NiObject)s); dropped++; }
                fin.RemoveUnreferencedBlocks();
                Console.WriteLine($"[headcarrier] keep1: kept {kept.Count} shape(s) ('{NameOf(keep)}' trigger{(kept.Count > 1 ? " + collision proxies" : "")}), dropped {dropped}");
            }
            if (anchorName != null)
            {
                var (moved, kept) = AnchorInMemory(fin, anchorName);
                Console.WriteLine($"[headcarrier] anchor '{anchorName}': moved {moved} branch(es), kept {kept} live-named at root");
            }
            if (xmlRel != null)
            {
                var ex = fin.Blocks.OfType<NiStringExtraData>().FirstOrDefault(b => b.Name?.String == HDT_EXTRA);
                if (ex == null)
                { Console.WriteLine($"[headcarrier] FAILED: no '{HDT_EXTRA}' extra data to re-point"); return 1; }
                Console.WriteLine($"[headcarrier] xml '{ex.StringData?.String}' -> '{xmlRel}'");
                ex.StringData = new NiStringRef(xmlRel);
            }
            if (!EnsureRootHdtExtra(fin))
            { Console.WriteLine($"[headcarrier] FAILED: no '{HDT_EXTRA}' extra data anywhere — not an SMP content set"); return 1; }
            if (fin.Save(outPath, new NifFileSaveOptions()) != 0)
            { Console.WriteLine($"[headcarrier] FAILED to save {outPath}"); return 1; }
            Console.WriteLine($"[headcarrier] wrote {outPath}");
        }
        finally { TryDelete(tMerged); TryDelete(tZa); }

        return ValidateHead(outPath);
    }

    // Collision-proxy companion NIF for the ExtraParts pattern: the engine
    // materializes only ONE geometry per head part (named after the editorID -
    // vanilla ships hair + hairline as SEPARATE linked parts for this reason),
    // so a collision proxy inside the carrier NIF never reaches the facegen
    // head and the physics XML's per-*-shape pair goes inert (in-game
    // 2026-07-04: veil collision dead with --proxyshader). Emit the proxies as
    // their OWN nif to hang off the carrier HDPT's ExtraParts: keep only the
    // shader-less SKINNED proxy shapes, give them the smallest shader-bearing
    // shape's shader block (borrowed BEFORE that shape is dropped), then
    // zero-alpha the result. No HDT extra data needed - the collision mesh is
    // resolved BY NAME across the whole facegen head by the carrier's system.
    static int ProxyNif(string inPath, string outPath)
    {
        var nif = new NifFile();
        if (nif.Load(inPath, new NifFileLoadOptions()) != 0 || !nif.Valid)
        { Console.WriteLine($"[proxynif] FAILED to load {inPath}"); return 1; }

        var all = nif.GetShapes().ToList();
        var donor = all
            .Where(s => s.HasSkinInstance && nif.GetShader(s) != null)
            .OrderBy(s => (int)s.VertexCount)
            .FirstOrDefault();
        var proxies = all.Where(s => s.HasSkinInstance && nif.GetShader(s) == null).ToList();
        if (donor == null || proxies.Count == 0)
        { Console.WriteLine($"[proxynif] FAILED: need a shader-bearing donor and >=1 shader-less skinned proxy (donor={(donor != null)}, proxies={proxies.Count})"); return 1; }

        int shaderId = -1;
        var donorRef = donor.GetType().GetProperty("ShaderPropertyRef")?.GetValue(donor);
        if (donorRef != null)
            shaderId = (int)(donorRef.GetType().GetProperty("Index")?.GetValue(donorRef) ?? -1);
        if (shaderId < 0)
        { Console.WriteLine("[proxynif] FAILED: cannot read donor shader ref"); return 1; }

        foreach (var p in proxies)
        {
            var prop = p.GetType().GetProperty("ShaderPropertyRef");
            if (prop == null || !prop.CanWrite)
            { Console.WriteLine($"[proxynif] FAILED: cannot set shader on '{NameOf(p)}' ({p.GetType().Name})"); return 1; }
            prop.SetValue(p, Activator.CreateInstance(prop.PropertyType, shaderId));
            Console.WriteLine($"[proxynif] proxy '{NameOf(p)}' borrows '{NameOf(donor)}' shader");
        }
        int dropped = 0;
        foreach (var s in all)
            if (!proxies.Contains(s)) { nif.RemoveBlock((NiObject)s); dropped++; }
        // Strip the inherited HDT extra data: the proxy must NOT declare its own
        // physics file, or FSMP creates a SECOND system for the same bones (the
        // carrier's xml diverges from the source once per-*-shape names are
        // rewritten, so physicsDupes would no longer dedupe them).
        foreach (var sed in nif.Blocks.OfType<NiStringExtraData>()
                     .Where(b => b.Name?.String == HDT_EXTRA).ToList())
        {
            nif.RemoveBlock(sed);
            Console.WriteLine("[proxynif] stripped inherited HDT physics extra data");
        }
        nif.RemoveUnreferencedBlocks();

        string tmp = outPath + ".px.nif";
        if (nif.Save(tmp, new NifFileSaveOptions()) != 0)
        { Console.WriteLine($"[proxynif] FAILED to save {tmp}"); TryDelete(tmp); return 1; }
        int rc = ZeroAlpha(tmp, outPath);  // invisible + NiAlphaProperty
        TryDelete(tmp);
        if (rc != 0) return rc;

        var chk = new NifFile();
        chk.Load(outPath, new NifFileLoadOptions());
        int shp = chk.GetShapes()?.Count() ?? 0;
        bool allShader = chk.GetShapes()?.All(s => chk.GetShader(s) != null) ?? false;
        Console.WriteLine($"[proxynif] VERDICT shapes={shp} (want {proxies.Count}) allShader={allShader} dropped={dropped}");
        return (shp == proxies.Count && allShader) ? 0 : 2;
    }

    // ---------------------------------------------------------------------------
    // approach-C persist pipeline (stage 3). The persist class rides the facegen
    // head path, whose PoC-proven constraints shape everything here:
    //   - the engine materializes ONE geometry per head part, renamed to the
    //     part's editorID -> every mesh the physics XML references must be its
    //     OWN head part, and the XML's per-*-shape names must be REWRITTEN to
    //     the pool editorIDs (C §9-18).
    //   - head parts are static ESP records -> a fixed POOL: CFW_PersistCarrier
    //     (bones + XML + trigger shape) and CFW_PersistProxy01..NN (one per
    //     additional collision/cloth mesh). sync assigns meshes to pool slots
    //     and CEF registers/repoints only the assigned ones (stage 3b).
    //   - every part NIF carries the full bone tree (merge is idempotent via
    //     FSMP's renameMap, and processing order of sibling parts is not
    //     guaranteed).
    const int kMaxPersistProxies = 8;
    const string kPersistCarrierEditorId = "CFW_PersistCarrier";

    static string PersistProxyEditorId(int i) => $"CFW_PersistProxy{i:00}";

    // Return the fragment with its "count" (SMP content count of the CURRENT
    // persist set) set/overwritten. CEF keys the head-part registration on this:
    // count!=0 -> register carrier+proxies, count==0 -> deregister. Older
    // fragments (pre-count) pass through unchanged on parse failure.
    static string FragmentWithCount(string fragment, int count)
    {
        if (fragment == null) return null;
        try
        {
            var node = System.Text.Json.Nodes.JsonNode.Parse(fragment);
            node["count"] = count;
            return node.ToJsonString();
        }
        catch { return fragment; }
    }

    // Rewrite per-*-shape name attributes per the pool assignment.
    static int RenameShapesInXml(string inPath, string outPath, Dictionary<string, string> renames)
    {
        var xdoc = System.Xml.Linq.XDocument.Load(inPath);
        if (xdoc.Root == null) return 1;
        int n = 0;
        foreach (var el in xdoc.Root.Elements())
        {
            var tag = el.Name.LocalName;
            if (tag != "per-vertex-shape" && tag != "per-triangle-shape") continue;
            var nm = el.Attribute("name")?.Value;
            if (nm != null && renames.TryGetValue(nm, out var to))
            {
                el.SetAttributeValue("name", to);
                n++;
            }
        }
        xdoc.Save(outPath);
        Console.WriteLine($"[persist] xml '{System.IO.Path.GetFileName(inPath)}': renamed {n} per-*-shape name(s)");
        return 0;
    }

    static string SyncPersistBuild(System.Text.Json.JsonElement persistEl, List<string> dataRoots,
        string carrierDir, string xmlDir, string tmpDir, string emptyNif, int kSlots,
        string oldFragment, ref int built, ref int skipped, ref int failed, ref int poolCreated)
    {
        string basePath = System.IO.Path.Combine(carrierDir, "Persist_carrier.nif");
        string hashPath = System.IO.Path.Combine(carrierDir, "Persist_carrier.hash");

        // Slot pool: placeholders must pre-exist for restart-free swaps (usvfs
        // cannot see files created after launch). Seeded from the base carrier
        // (or the empty token nif) - unused proxies are never REGISTERED, so
        // their placeholder content is irrelevant.
        int localPool = 0;
        void EnsurePool()
        {
            string seed = System.IO.File.Exists(basePath) ? basePath : emptyNif;
            for (int k = 0; k < kSlots; k++)
            {
                string cn = System.IO.Path.Combine(carrierDir, $"Persist_carrier_r{k}.nif");
                if (!System.IO.File.Exists(cn) && seed != null) { System.IO.File.Copy(seed, cn); localPool++; }
                for (int i = 1; i <= kMaxPersistProxies; i++)
                {
                    string pn = System.IO.Path.Combine(carrierDir, $"Persist_part{i:00}_r{k}.nif");
                    if (!System.IO.File.Exists(pn) && seed != null) { System.IO.File.Copy(seed, pn); localPool++; }
                }
                string sx = System.IO.Path.Combine(xmlDir, $"Persist_physics_r{k}.xml");
                if (!System.IO.File.Exists(sx))
                { System.IO.File.WriteAllText(sx, "<?xml version=\"1.0\"?>\n<system></system>\n"); localPool++; }
            }
        }

        // Resolve contents - EXCLUDE (warn + drop), never fail the whole set.
        // Distinguish TRANSIENT misses (path resolution - a data-root/VFS hiccup
        // may heal on the next run) from DECISIVE exclusions (non-SMP content /
        // validation reject): only a decisively-empty set may flip count to 0.
        var smp = new List<(string nif, string xmlDisk, string xmlRel)>();
        int declared = 0, transientMiss = 0;
        if (persistEl.ValueKind == System.Text.Json.JsonValueKind.Object &&
            persistEl.TryGetProperty("contents", out var pcontents))
        {
            declared = pcontents.GetArrayLength();
            foreach (var c in pcontents.EnumerateArray())
            {
                string rel = c.GetProperty("nif").GetString();
                string nifDisk = ResolveAgainstRoots(rel, dataRoots, meshesRel: true);
                if (nifDisk == null)
                { Console.WriteLine($"[persist] WARNING content '{rel}' EXCLUDED — cannot resolve under data roots"); transientMiss++; continue; }
                string xmlRel = GetHdtXmlPath(nifDisk);
                if (xmlRel == null) continue; // not an SMP content
                if (!ValidateNifPath(nifDisk, out string reason))
                { Console.WriteLine($"[persist] WARNING content '{rel}' EXCLUDED — {reason}"); continue; }
                string xmlDisk = ResolveAgainstRoots(xmlRel, dataRoots, meshesRel: false);
                if (xmlDisk == null)
                { Console.WriteLine($"[persist] WARNING content '{rel}' EXCLUDED — physics xml '{xmlRel}' not found"); transientMiss++; continue; }
                smp.Add((nifDisk, xmlDisk, xmlRel));
            }
        }

        if (smp.Count == 0)
        {
            EnsurePool();
            poolCreated += localPool;
            if (transientMiss > 0)
            {
                // Guard against clobbering on a transient path miss, mirroring
                // the box behavior: keep previous artifacts AND count.
                Console.WriteLine($"[persist] {transientMiss} content(s) unresolved (transient?) — keeping previous artifacts");
                return oldFragment;
            }
            // Decisively no SMP persist content: artifacts stay (rev continuity),
            // but count goes 0 so CEF deregisters the head-part pool.
            if (declared > 0)
                Console.WriteLine($"[persist] no SMP content among {declared} declared — count=0 (CEF deregisters)");
            return FragmentWithCount(oldFragment, 0);
        }

        // Hash inputs; skip when unchanged
        var h = new System.Text.StringBuilder("p1|");
        foreach (var s in smp.OrderBy(x => x.nif))
        {
            var fi = new System.IO.FileInfo(s.nif);
            var xi = new System.IO.FileInfo(s.xmlDisk);
            h.Append($"{s.nif}|{fi.Length}|{fi.LastWriteTimeUtc.Ticks}|{s.xmlDisk}|{xi.Length}|{xi.LastWriteTimeUtc.Ticks}|");
        }
        string hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(
            System.Text.Encoding.UTF8.GetBytes(h.ToString())));
        if (System.IO.File.Exists(hashPath) && System.IO.File.Exists(basePath)
            && System.IO.File.ReadAllText(hashPath) == hash && oldFragment != null)
        { EnsurePool(); poolCreated += localPool; skipped++; return FragmentWithCount(oldFragment, smp.Count); }

        Console.WriteLine($"[persist] {smp.Count} SMP content(s)");

        // Base ordering: a trigger-owning content first (its shapes load
        // in-place and survive another content's bones-only fallback).
        int baseIdx = -1;
        for (int i = 0; i < smp.Count && baseIdx < 0; i++)
        {
            var n = new NifFile();
            if (n.Load(smp[i].nif, new NifFileLoadOptions()) != 0 || !n.Valid) continue;
            foreach (var s in n.GetShapes())
                if (s.HasSkinInstance && n.GetShader(s) != null &&
                    n.GetShapeBoneNames(s).Any(b => !LooksLiveSkeletonBone(b)))
                { baseIdx = i; break; }
        }
        if (baseIdx < 0)
        { Console.WriteLine("[persist] FAILED: no content owns a merge-trigger shape (R2)"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
        var ordered = new List<(string nif, string xmlDisk, string xmlRel)> { smp[baseIdx] };
        ordered.AddRange(smp.Where((_, i) => i != baseIdx));

        // Master merged nif: bones union + every content's shapes (zero-alpha'd),
        // R3-completed, root HDT extra ensured. Parts are pruned copies of this.
        string tMg = System.IO.Path.Combine(tmpDir, "persist_mg.nif");
        string tZa = System.IO.Path.Combine(tmpDir, "persist_za.nif");
        string tBase = System.IO.Path.Combine(tmpDir, "persist_base.nif");
        int rc = 0;
        string cur = ordered[0].nif;
        if (ordered.Count > 1)
        {
            rc = Merge(tMg, ordered.Select(s => s.nif).ToArray());
            cur = tMg;
        }
        if (rc == 0) rc = ZeroAlpha(cur, tZa);
        if (rc != 0)
        { Console.WriteLine("[persist] FAILED building the master merge"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
        {
            var fin = new NifFile();
            if (fin.Load(tZa, new NifFileLoadOptions()) != 0 || !fin.Valid)
            { Console.WriteLine("[persist] FAILED to reload master merge"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
            CompleteBoneTree(fin);
            if (!EnsureRootHdtExtra(fin))
            { Console.WriteLine("[persist] FAILED: no HDT extra data in the merged set"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
            if (fin.Save(tBase, new NifFileSaveOptions()) != 0)
            { Console.WriteLine("[persist] FAILED to save master merge"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
        }

        // Needed head meshes = every per-*-shape name across the contents' XMLs.
        var needed = new List<string>();
        foreach (var xmlDisk in ordered.Select(s => s.xmlDisk).Distinct())
        {
            try
            {
                var xdoc = System.Xml.Linq.XDocument.Load(xmlDisk);
                foreach (var el in xdoc.Root.Elements())
                {
                    var tag = el.Name.LocalName;
                    if (tag != "per-vertex-shape" && tag != "per-triangle-shape") continue;
                    var nm = el.Attribute("name")?.Value;
                    if (nm != null && !needed.Contains(nm)) needed.Add(nm);
                }
            }
            catch (Exception e)
            { Console.WriteLine($"[persist] WARNING cannot parse xml '{xmlDisk}': {e.Message}"); }
        }

        // Shape inventory of the master merge
        var inv = new NifFile();
        inv.Load(tBase, new NifFileLoadOptions());
        var shapeInfo = new Dictionary<string, (bool skinned, bool shader, bool custom)>();
        foreach (var s in inv.GetShapes())
        {
            string nm = NameOf(s) ?? "";
            bool skinned = s.HasSkinInstance;
            bool shader = inv.GetShader(s) != null;
            bool custom = skinned && inv.GetShapeBoneNames(s).Any(b => !LooksLiveSkeletonBone(b));
            shapeInfo[nm] = (skinned, shader, custom);
        }

        // Assignment: carrier gets a trigger-qualifying mesh (prefer one the XML
        // references, so its rename doubles as the cloth body); remaining needed
        // meshes go to the proxy pool in order.
        string carrierMesh = needed.FirstOrDefault(nm =>
            shapeInfo.TryGetValue(nm, out var si) && si.skinned && si.custom);
        if (carrierMesh == null)
            carrierMesh = shapeInfo.FirstOrDefault(kv => kv.Value.skinned && kv.Value.custom).Key;
        if (carrierMesh == null)
        { Console.WriteLine("[persist] FAILED: master merge has no trigger shape (R2)"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }

        var assign = new List<(string mesh, string editorId)> { (carrierMesh, kPersistCarrierEditorId) };
        int pi = 1;
        foreach (var nm in needed)
        {
            if (nm == carrierMesh) continue;
            if (!shapeInfo.TryGetValue(nm, out var si) || !si.skinned)
            { Console.WriteLine($"[persist] WARNING collision mesh '{nm}' has no skinned shape in the merge — its collision goes inert"); continue; }
            if (pi > kMaxPersistProxies)
            { Console.WriteLine($"[persist] WARNING proxy pool exhausted ({kMaxPersistProxies}) — collision mesh '{nm}' goes inert"); continue; }
            assign.Add((nm, PersistProxyEditorId(pi)));
            pi++;
        }
        foreach (var (mesh, eid) in assign)
            Console.WriteLine($"[persist] assign '{mesh}' -> {eid}");

        // XML: per-content rename -> merge -> temp
        var renames = assign.ToDictionary(a => a.mesh, a => a.editorId);
        var txmls = new List<string>();
        int ti = 0;
        foreach (var xmlDisk in ordered.Select(s => s.xmlDisk).Distinct())
        {
            string t = System.IO.Path.Combine(tmpDir, $"persist_x{ti++}.xml");
            if (RenameShapesInXml(xmlDisk, t, renames) != 0)
            { Console.WriteLine($"[persist] FAILED xml transform '{xmlDisk}'"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
            txmls.Add(t);
        }
        string tXml = System.IO.Path.Combine(tmpDir, "persist_physics.xml");
        if (txmls.Count == 1) System.IO.File.Copy(txmls[0], tXml, overwrite: true);
        else if (MergeXml(tXml, txmls.ToArray()) != 0)
        { Console.WriteLine("[persist] FAILED xml merge"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }

        // Revision + slot paths
        int rev = 1;
        if (oldFragment != null)
        {
            try { rev = System.Text.Json.JsonDocument.Parse(oldFragment).RootElement.GetProperty("rev").GetInt32() + 1; }
            catch { }
        }
        int slotIdx = rev % kSlots;
        string slotXmlDisk = System.IO.Path.Combine(xmlDir, $"Persist_physics_r{slotIdx}.xml");
        string slotXmlRel = $"meshes\\CostumeFW\\XML\\Persist_physics_r{slotIdx}.xml";

        // Build every part into tmp, validate, then publish the whole set.
        var partFiles = new List<(string editorId, string tmp, string slotDisk, string slotRel)>();
        foreach (var (mesh, editorId) in assign)
        {
            bool isCarrier = editorId == kPersistCarrierEditorId;
            var part = new NifFile();
            if (part.Load(tBase, new NifFileLoadOptions()) != 0 || !part.Valid)
            { Console.WriteLine("[persist] FAILED to reload master for part build"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
            var all = part.GetShapes().ToList();
            var target = all.FirstOrDefault(s => (NameOf(s) ?? "") == mesh);
            if (target == null)
            { Console.WriteLine($"[persist] FAILED: shape '{mesh}' missing from master"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
            // Borrow a shader BEFORE dropping the donor (R4: facegen breaks on
            // shader-less geometry).
            if (part.GetShader(target) == null)
            {
                var donor = all.FirstOrDefault(s => part.GetShader(s) != null);
                var dref = donor?.GetType().GetProperty("ShaderPropertyRef")?.GetValue(donor);
                int sid = dref != null ? (int)(dref.GetType().GetProperty("Index")?.GetValue(dref) ?? -1) : -1;
                var prop = target.GetType().GetProperty("ShaderPropertyRef");
                if (sid < 0 || prop == null || !prop.CanWrite)
                { Console.WriteLine($"[persist] FAILED: cannot shader-fix proxy '{mesh}'"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
                prop.SetValue(target, Activator.CreateInstance(prop.PropertyType, sid));
            }
            foreach (var s in all)
                if (!ReferenceEquals(s, target)) part.RemoveBlock((NiObject)s);
            if (isCarrier)
            {
                var ex = part.Blocks.OfType<NiStringExtraData>().FirstOrDefault(b => b.Name?.String == HDT_EXTRA);
                if (ex == null)
                { Console.WriteLine("[persist] FAILED: carrier lost its HDT extra data"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
                ex.StringData = new NiStringRef(slotXmlRel);
            }
            else
            {
                foreach (var sed in part.Blocks.OfType<NiStringExtraData>()
                             .Where(b => b.Name?.String == HDT_EXTRA).ToList())
                    part.RemoveBlock(sed);
            }
            part.RemoveUnreferencedBlocks();

            string tOut = System.IO.Path.Combine(tmpDir, $"persist_{editorId}.nif");
            if (part.Save(tOut, new NifFileSaveOptions()) != 0)
            { Console.WriteLine($"[persist] FAILED to save part '{editorId}'"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }
            if (!ValidateNifPath(tOut, out string preason))
            { Console.WriteLine($"[persist] FAILED: part '{editorId}' rejected — {preason}"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }

            string slotDisk, slotRel;
            if (isCarrier)
            {
                slotDisk = System.IO.Path.Combine(carrierDir, $"Persist_carrier_r{slotIdx}.nif");
                slotRel = $"CostumeFW/Persist_carrier_r{slotIdx}.nif";
            }
            else
            {
                string idx = editorId.Substring(editorId.Length - 2);
                slotDisk = System.IO.Path.Combine(carrierDir, $"Persist_part{idx}_r{slotIdx}.nif");
                slotRel = $"CostumeFW/Persist_part{idx}_r{slotIdx}.nif";
            }
            partFiles.Add((editorId, tOut, slotDisk, slotRel));
        }

        // Final gate on the carrier part (R1-R4), then publish atomically.
        string carrierTmp = partFiles.First(p => p.editorId == kPersistCarrierEditorId).tmp;
        if (ValidateHead(carrierTmp) != 0)
        { Console.WriteLine("[persist] FAILED: carrier part rejected by validatehead — previous artifacts kept"); failed++; EnsurePool(); poolCreated += localPool; return oldFragment; }

        System.IO.File.Copy(tXml, slotXmlDisk, overwrite: true);
        foreach (var p in partFiles)
            System.IO.File.Copy(p.tmp, p.slotDisk, overwrite: true);
        System.IO.File.Copy(carrierTmp, basePath, overwrite: true);
        System.IO.File.WriteAllText(hashPath, hash);
        built++;
        EnsurePool();
        poolCreated += localPool;

        // Fragment: rev/file mirror the box entry shape (tolerant readers), plus
        // the persist-specific xml + parts assignment for CEF's registration.
        var frag = new System.Text.StringBuilder();
        frag.Append("{ \"rev\": ").Append(rev)
            .Append(", \"count\": ").Append(smp.Count)
            .Append(", \"file\": \"").Append(partFiles.First(p => p.editorId == kPersistCarrierEditorId).slotRel)
            .Append("\", \"xml\": \"").Append(slotXmlRel.Replace("\\", "\\\\"))
            .Append("\", \"parts\": [");
        bool pf = true;
        foreach (var p in partFiles.Where(p => p.editorId != kPersistCarrierEditorId))
        {
            if (!pf) frag.Append(", ");
            pf = false;
            frag.Append("{ \"editorid\": \"").Append(p.editorId).Append("\", \"file\": \"").Append(p.slotRel).Append("\" }");
        }
        frag.Append("] }");
        Console.WriteLine($"[persist] rev={rev} -> {partFiles.Count} part(s), xml={slotXmlRel}");
        return frag.ToString();
    }

    static int Sync(string[] args)
    {
        string manifestPath = null, outRoot = null, emptyNif = null, mo2Root = null, profile = "Default";
        var dataRoots = new List<string>();
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--data": dataRoots.Add(args[++i]); break;
                case "--mo2": mo2Root = args[++i]; break;
                case "--profile": profile = args[++i]; break;
                case "--out": outRoot = args[++i]; break;
                case "--empty": emptyNif = args[++i]; break;
                default: manifestPath ??= args[i]; break;
            }
        }
        if (mo2Root != null)
        {
            // Resolve data roots from the MO2 profile: overwrite first (top
            // priority), then every ENABLED mod in modlist.txt order — the file
            // lists mods winners-first (verified against a live instance), which
            // matches first-hit resolution. Explicit --data roots stay ahead as
            // deliberate overrides.
            string ml = System.IO.Path.Combine(mo2Root, "profiles", profile, "modlist.txt");
            if (!System.IO.File.Exists(ml))
            { Console.WriteLine($"[sync] --mo2: modlist not found: {ml}"); return 1; }
            var mo2Roots = new List<string> { System.IO.Path.Combine(mo2Root, "overwrite") };
            foreach (var line in System.IO.File.ReadLines(ml))
            {
                if (!line.StartsWith("+")) continue;                 // enabled mods only
                string name = line.Substring(1);
                if (name.EndsWith("_separator")) continue;
                string root = System.IO.Path.Combine(mo2Root, "mods", name);
                if (System.IO.Directory.Exists(root)) mo2Roots.Add(root);
            }
            dataRoots.AddRange(mo2Roots);
            Console.WriteLine($"[sync] --mo2: {mo2Roots.Count - 1} enabled mod root(s) (winners first) + overwrite, profile '{profile}'");
        }
        if (manifestPath == null || outRoot == null || dataRoots.Count == 0)
        { Console.WriteLine("[sync] usage: sync <manifest.json> (--mo2 <instanceRoot> [--profile <name>] | --data <root> [--data ...]) --out <cefModRoot> [--empty <emptyToken.nif>]"); return 1; }

        var doc = System.Text.Json.JsonDocument.Parse(System.IO.File.ReadAllText(manifestPath));
        string carrierDir = System.IO.Path.Combine(outRoot, "meshes", "CostumeFW");
        string xmlDir = System.IO.Path.Combine(carrierDir, "XML");
        string tmpDir = System.IO.Path.Combine(outRoot, ".cef_tmp");
        System.IO.Directory.CreateDirectory(carrierDir);
        System.IO.Directory.CreateDirectory(xmlDir);
        System.IO.Directory.CreateDirectory(tmpDir);

        // --- revision slots (restart-free carrier swaps) -----------------------
        // MO2's usvfs does NOT see files an external process creates after game
        // launch, but DOES see external rewrites of pre-existing files (verified
        // in-game 2026-07-03). So each box gets a pre-created pool of slot files;
        // a rebuild bumps the box's revision and rewrites slot r(rev % N). CEF
        // reads carriers.json and repoints the token ARMA at the new slot path -
        // a path the engine hasn't cached this session - then re-equips.
        const int kSlots = 8;
        string carriersJsonPath = System.IO.Path.Combine(carrierDir, "carriers.json");
        var carriers = new System.Collections.Generic.Dictionary<string, System.Collections.Generic.Dictionary<string, object>>();
        // The persist entry has a RICHER shape (parts assignment) than box
        // entries, so it round-trips as a raw JSON fragment: parsing it into
        // the {rev,file} dict would silently drop the parts on the next
        // skip-path rewrite.
        string persistFragment = null;
        if (System.IO.File.Exists(carriersJsonPath))
        {
            try
            {
                foreach (var p in System.Text.Json.JsonDocument.Parse(System.IO.File.ReadAllText(carriersJsonPath)).RootElement.EnumerateObject())
                {
                    if (p.Name == "persist")
                    {
                        persistFragment = p.Value.GetRawText();
                        continue;
                    }
                    carriers[p.Name] = new System.Collections.Generic.Dictionary<string, object> {
                        ["rev"] = p.Value.GetProperty("rev").GetInt32(),
                        ["file"] = p.Value.GetProperty("file").GetString()
                    };
                }
            }
            catch { Console.WriteLine("[sync] WARNING: carriers.json unreadable, starting fresh"); }
        }
        int poolCreated = 0;

        int built = 0, skipped = 0, failed = 0;
        foreach (var box in doc.RootElement.GetProperty("boxes").EnumerateArray())
        {
            int slot = box.GetProperty("slot").GetInt32();
            string carrierPath = System.IO.Path.Combine(carrierDir, $"Box{slot}_carrier.nif");
            string mergedXmlDisk = System.IO.Path.Combine(xmlDir, $"Box{slot}_physics.xml");
            string mergedXmlRel = $"meshes\\CostumeFW\\XML\\Box{slot}_physics.xml";
            string hashPath = System.IO.Path.Combine(carrierDir, $"Box{slot}_carrier.hash");

            // Collect SMP contents: (resolved nif, resolved xml)
            // A content that can't be resolved/validated is EXCLUDED (warn + drop),
            // NOT failed - one broken content must never sink the whole box; the
            // carrier is rebuilt from the rest, mirroring the Defense-B exclusion.
            var contentsArr = box.GetProperty("contents");
            int declared = contentsArr.GetArrayLength();
            var smp = new List<(string nif, string xmlDisk, string xmlRel)>();
            foreach (var c in contentsArr.EnumerateArray())
            {
                string rel = c.GetProperty("nif").GetString();
                string nifDisk = ResolveAgainstRoots(rel, dataRoots, meshesRel: true);
                if (nifDisk == null)
                { Console.WriteLine($"[sync] box{slot}: WARNING content '{rel}' EXCLUDED — cannot resolve under data roots; box built from the rest"); continue; }
                string xmlRel = GetHdtXmlPath(nifDisk);
                if (xmlRel == null) continue; // not an SMP content
                // Defense B: exclude a content whose skin data would crash the
                // engine's partition loader, rather than baking it into the carrier.
                // The box is still rebuilt from the remaining (valid) contents.
                if (!ValidateNifPath(nifDisk, out string contentReason))
                {
                    Console.WriteLine($"[sync] box{slot}: WARNING content '{rel}' EXCLUDED — {contentReason} (would crash the SSE skin-partition loader); box built from the rest");
                    continue;
                }
                string xmlDisk2 = ResolveAgainstRoots(xmlRel, dataRoots, meshesRel: false);
                if (xmlDisk2 == null)
                { Console.WriteLine($"[sync] box{slot}: WARNING content '{rel}' EXCLUDED — physics xml '{xmlRel}' not found under data roots; box built from the rest"); continue; }
                smp.Add((nifDisk, xmlDisk2, xmlRel));
            }

            // A box that DECLARED contents but resolved none must not clobber a good
            // carrier down to empty - that would strip physics on a transient path
            // miss. Keep the last good build (Defense A/B: never lose it). Genuinely
            // empty boxes (declared == 0) still fall through to the --empty template.
            if (declared > 0 && smp.Count == 0 && System.IO.File.Exists(carrierPath))
            {
                Console.WriteLine($"[sync] box{slot}: all {declared} declared content(s) unresolved/excluded — keeping previous carrier");
                EnsurePool(); skipped++; continue;
            }

            // Hash inputs; skip when unchanged
            var h = new System.Text.StringBuilder("v1|");
            foreach (var s in smp.OrderBy(x => x.nif))
            {
                var fi = new System.IO.FileInfo(s.nif);
                var xi = new System.IO.FileInfo(s.xmlDisk);
                h.Append($"{s.nif}|{fi.Length}|{fi.LastWriteTimeUtc.Ticks}|{s.xmlDisk}|{xi.Length}|{xi.LastWriteTimeUtc.Ticks}|");
            }
            string hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(
                System.Text.Encoding.UTF8.GetBytes(h.ToString())));
            // Slot-pool ensure runs on BOTH paths (skip included): placeholders must
            // exist before the next launch for restart-free swaps to be possible.
            void EnsurePool()
            {
                for (int i = 0; i < kSlots; i++)
                {
                    string sn = System.IO.Path.Combine(carrierDir, $"Box{slot}_carrier_r{i}.nif");
                    string sx = System.IO.Path.Combine(xmlDir, $"Box{slot}_physics_r{i}.xml");
                    string seed = System.IO.File.Exists(carrierPath) ? carrierPath : emptyNif;
                    if (!System.IO.File.Exists(sn) && seed != null)
                    { System.IO.File.Copy(seed, sn); poolCreated++; }
                    if (!System.IO.File.Exists(sx))
                    { System.IO.File.WriteAllText(sx, "<?xml version=\"1.0\"?>\n<system></system>\n"); poolCreated++; }
                }
                if (!carriers.ContainsKey(slot.ToString()))
                    carriers[slot.ToString()] = new System.Collections.Generic.Dictionary<string, object>
                    { ["rev"] = 0, ["file"] = $"CostumeFW/Box{slot}_carrier.nif" };
            }

            if (System.IO.File.Exists(hashPath) && System.IO.File.Exists(carrierPath)
                && System.IO.File.ReadAllText(hashPath) == hash)
            { EnsurePool(); skipped++; continue; }

            Console.WriteLine($"[sync] box{slot}: {smp.Count} SMP content(s)");
            int rc;
            // Build into a temp, VALIDATE, then atomically publish to the base
            // carrier - so a bad build can never clobber the last good carrier
            // that a worn box token already loads.
            string outTmp = System.IO.Path.Combine(tmpDir, $"box{slot}_out.nif");
            if (smp.Count == 0)
            {
                if (emptyNif == null || !System.IO.File.Exists(emptyNif))
                { Console.WriteLine($"[sync] box{slot}: no SMP contents and no --empty template — skipped"); failed++; continue; }
                System.IO.File.Copy(emptyNif, outTmp, overwrite: true);
                rc = 0;
            }
            else if (smp.Count == 1)
            {
                rc = ZeroAlpha(smp[0].nif, outTmp);
            }
            else
            {
                // merge FIRST (bone union + every content's shapes cloned in), then
                // zero-alpha the whole merged NIF so cloned shapes go invisible too,
                // then point the extra data at the unified XML.
                string t1 = System.IO.Path.Combine(tmpDir, $"box{slot}_mg.nif");
                string t2 = System.IO.Path.Combine(tmpDir, $"box{slot}_za.nif");
                rc = Merge(t1, smp.Select(s => s.nif).ToArray());
                if (rc == 0) rc = ZeroAlpha(t1, t2);
                if (rc == 0) rc = MergeXml(mergedXmlDisk, smp.Select(s => s.xmlDisk).ToArray());
                if (rc == 0) rc = SetXml(t2, outTmp, mergedXmlRel);
            }
            // Defense A: final gate. Never publish a carrier the SSE loader would
            // divide-by-zero on (catches anything the merge itself introduces).
            if (rc == 0 && !ValidateNifPath(outTmp, out string builtReason))
            {
                Console.WriteLine($"[sync] box{slot}: BUILT CARRIER REJECTED — {builtReason}; keeping previous carrier, revision NOT bumped");
                rc = 3;
            }
            if (rc != 0) { failed++; Console.WriteLine($"[sync] box{slot}: FAILED (rc={rc})"); continue; }

            // Publish the validated build to the base carrier, then record the hash.
            System.IO.File.Copy(outTmp, carrierPath, overwrite: true);
            System.IO.File.WriteAllText(hashPath, hash);
            built++;

            EnsurePool();

            // Bump revision and rewrite this box's active slot with the new build.
            int rev = carriers.TryGetValue(slot.ToString(), out var prev) ? (int)prev["rev"] + 1 : 1;
            int slotIdx = rev % kSlots;
            string slotNifDisk = System.IO.Path.Combine(carrierDir, $"Box{slot}_carrier_r{slotIdx}.nif");
            string slotNifRel = $"CostumeFW/Box{slot}_carrier_r{slotIdx}.nif";
            if (smp.Count >= 2)
            {
                // Slot carrier must reference the slot XML (the merged XML content
                // changed too, and FSMP may cache XML by path).
                string slotXmlDisk = System.IO.Path.Combine(xmlDir, $"Box{slot}_physics_r{slotIdx}.xml");
                string slotXmlRel = $"meshes\\CostumeFW\\XML\\Box{slot}_physics_r{slotIdx}.xml";
                System.IO.File.Copy(mergedXmlDisk, slotXmlDisk, overwrite: true);
                if (SetXml(carrierPath, slotNifDisk, slotXmlRel) != 0)
                { Console.WriteLine($"[sync] box{slot}: WARNING slot carrier setxml failed"); }
            }
            else
            {
                System.IO.File.Copy(carrierPath, slotNifDisk, overwrite: true);
            }
            carriers[slot.ToString()] = new System.Collections.Generic.Dictionary<string, object>
            { ["rev"] = rev, ["file"] = slotNifRel };
            Console.WriteLine($"[sync] box{slot}: rev={rev} -> {slotNifRel}");
        }

        // approach-C persist pipeline (after boxes: shares the tmp dir + pools)
        if (doc.RootElement.TryGetProperty("persist", out var persistEl))
        {
            persistFragment = SyncPersistBuild(persistEl, dataRoots, carrierDir, xmlDir, tmpDir,
                emptyNif, kSlots, persistFragment, ref built, ref skipped, ref failed, ref poolCreated);
        }

        // carriers.json: always rewrite (pre-existing file = externally-visible swap signal)
        var jsonOut = new System.Text.StringBuilder("{\n");
        bool first = true;
        foreach (var kv in carriers.OrderBy(k => k.Key))
        {
            if (!first) jsonOut.Append(",\n");
            first = false;
            jsonOut.Append($" \"{kv.Key}\": {{ \"rev\": {kv.Value["rev"]}, \"file\": \"{kv.Value["file"]}\" }}");
        }
        if (persistFragment != null)
        {
            if (!first) jsonOut.Append(",\n");
            first = false;
            jsonOut.Append($" \"persist\": {persistFragment}");
        }
        jsonOut.Append("\n}\n");
        System.IO.File.WriteAllText(carriersJsonPath, jsonOut.ToString());

        try { System.IO.Directory.Delete(tmpDir, recursive: true); } catch { }
        if (poolCreated > 0)
            Console.WriteLine($"[sync] NOTE: created {poolCreated} slot placeholder file(s) - a running game cannot see NEW files; they register on the next launch");
        Console.WriteLine($"[sync] done: built={built} skipped(unchanged)={skipped} failed={failed}");
        return failed == 0 ? 0 : 2;
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
            Console.WriteLine("       nifcarrier mergexml   <out.xml> <in1.xml> <in2.xml> [...]   (unified SMP physics XML)");
            Console.WriteLine("       nifcarrier setxml     <in.nif> <out.nif> <xmlPath>   (re-point HDT extra data at an XML)");
            Console.WriteLine("       nifcarrier sync       <manifest.json> --data <root> [--data ...] --out <cefModRoot> [--empty <token.nif>]");
            Console.WriteLine("       nifcarrier anchor     <in.nif> <out.nif> <anchorBoneName>");
            Console.WriteLine("       nifcarrier anchorsmart <in.nif> <out.nif> <anchorBoneName>   (reuses existing node; leaves live-named branches)");
            Console.WriteLine("       nifcarrier validate   <nif>   (is it safe to bake into an SSE carrier? divide-by-zero check)");
            Console.WriteLine("       nifcarrier validatehead <nif>   (approach-C gates: R1 root-xml / R2 merge-trigger / R3 tree-reachable)");
            Console.WriteLine("       nifcarrier headcarrier <out.nif> <content.nif> [c2 ...] [--xml <gameRelXml>] [--anchor <liveBone>]   (approach-C facegen carrier)");
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
                case "mergexml": return MergeXml(args[1], args.Skip(2).ToArray());
                case "setxml": return SetXml(args[1], args[2], args[3]);
                case "validate": return Validate(args[1]);
                case "sync": return Sync(args.Skip(1).ToArray());
                case "anchor": return Anchor(args[1], args[2], args[3]);
                case "anchorsmart": return AnchorSmart(args[1], args[2], args[3]);
                case "validatehead": return ValidateHead(args[1]);
                case "headcarrier": return HeadCarrier(args.Skip(1).ToArray());
                case "proxynif": return ProxyNif(args[1], args[2]);
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

#pragma once

namespace RE
{
    class TESObjectREFR;
    class NiAVObject;
}

// Thin wrapper over skee64 (RaceMenu) IBodyMorphInterface. Lets us apply the
// player's BodyMorph (RaceMenu sliders / BodyGen / OBody) vertex deltas to our
// INJECTED costume meshes - which bypass the equip system and so are never seen
// by skee's normal on-equip morph pass. Without this the costume keeps its base
// (un-morphed) shape and clips through a morphed 3BA/CBBE body.
namespace CostumeFW::BodyMorph
{
    // Acquire skee's IBodyMorphInterface via the SKSE interface exchange. Call
    // once at the SKSE kPostLoad message (after skee has registered). Safe to
    // call when skee is absent (leaves the feature disabled).
    void RequestInterface();

    // True if skee is present and the BodyMorph interface was acquired.
    bool Available();

    // Apply the actor's body morph vertex diff to the skinned geometry under
    // a_node (an injected holder). No-op if skee is absent or the actor has no
    // morphs. Must run after the node is attached + updated. Main thread.
    void ApplyToNode(RE::TESObjectREFR* a_refr, RE::NiAVObject* a_node);
}

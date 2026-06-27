#pragma once

// Engine functions resolved by Address Library ID (verified for 1.6.1170).
namespace CostumeFW::Offsets
{
    // NiSkinInstance::UpdateBoneMatrices - recomputes the bone matrix cache from
    // the (possibly rebound) bones[] against a root transform. Without this the
    // deform is stale / CTDs on next render.
    //   SE id 75655 / AE id 77461  ->  1.6.1170 offset 0xE4FF90 (verified via
    //   versionlib-1-6-1170-0.bin parse). Matches ersh1/Precision Offsets.h.
    inline REL::Relocation<void (*)(RE::NiSkinInstance*, RE::NiTransform&)>
        UpdateBoneMatrices{ RELOCATION_ID(75655, 77461) };

    // NOTE: community REL IDs 99866/106432 (InitializeShader) + 99865/106431
    // (InvalidateTextures) RESOLVE on 1.6.1170 but CRASH when called on a cloned
    // envmap material (access violation in BSLightingShader). Do not reuse them;
    // texture variants must come from the ARMA alternate-texture set instead.
}

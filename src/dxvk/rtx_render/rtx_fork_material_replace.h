// src/dxvk/rtx_render/rtx_fork_material_replace.h
//
// Fork-owned, pure (no Remix type): what an API draw looks up in the mods' material replacements, and when.
// [nfs2se 0041] Host test: tools/remix-plus/test/test_material_replace.cpp (+ mutants, run-tests.sh).
//
// A D3D9 draw takes the mod material `mat_<hash>` whose hash is its LEGACY texture hash (the colour texture's image hash:
// SceneManager::determineMaterialData -> AssetReplacer::getReplacementMaterial(legacyMaterial.getHash())) and the instance
// categories of every rtx.*Textures list by that hash (DrawCallState::setupCategoriesForTexture). An API draw has no legacy
// material: its own material hash is a hash of constants and textures, so neither lookup ever matched. The legacy hash of an
// API material is the image hash of its albedo texture (an API-uploaded texture's image hash is the host's `hash`), or the one
// the host states in remixapi_MaterialInfoReplaceEXT.
#pragma once

#include <cstdint>

namespace dxvk {
namespace fork_material_replace {

  // remixapi_MaterialInfoReplaceEXT::flags
  constexpr uint32_t kKeepRoughness = 1u << 0;  // this material's roughness constant wins over the replacement's (its roughness map is dropped)
  constexpr uint32_t kKeepMetallic  = 1u << 1;  // likewise for the metallic constant and map
  constexpr uint32_t kNoReplace     = 1u << 2;  // no mod material for this one (its legacy hash still gives the list categories)

  struct Request {
    uint64_t legacyHash = 0;  // 0 = derive it from the albedo texture's image hash
    uint32_t flags = 0;
  };

  struct Plan {
    uint64_t hash = 0;         // the legacy hash of the draw (0 = none): the category lists and the replacement are looked up by it
    bool replace = false;      // look the replacement up by `hash`
    bool explicitHash = false; // the host stated the hash: a missing replacement is a warning there, the normal case otherwise
  };

  // `option` = rtx.capiMaterialReplacement; `req` = the host's EXT for this material (null = none); `albedoHash` = the image hash of the API
  // material's albedo texture (0 = none, or not an opaque material).
  inline Plan plan(bool option, const Request* req, uint64_t albedoHash) {
    Plan p;
    p.explicitHash = req && req->legacyHash != 0;
    p.hash = p.explicitHash ? req->legacyHash : albedoHash;
    p.replace = option && p.hash != 0 && !(req && (req->flags & kNoReplace));
    return p;
  }

} // namespace fork_material_replace
} // namespace dxvk

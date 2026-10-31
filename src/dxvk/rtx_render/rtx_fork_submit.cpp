// src/dxvk/rtx_render/rtx_fork_submit.cpp
//
// Fork-owned file. Contains the implementations of fork_hooks:: functions
// for the SceneManager::submitExternalDraw path, lifted from
// rtx_scene_manager.cpp during the 2026-04-18 fork touchpoint-pattern refactor.
//
// See docs/fork-touchpoints.md for the full fork-hooks catalogue.
//
// NOTE: externalDrawObjectPicking accesses SceneManager::m_drawCallMeta, which
// is a private member. This file requires that SceneManager declare
// fork_hooks::externalDrawObjectPicking as a friend, OR that DrawCallMetaInfo
// and m_drawCallMeta be made accessible via a public accessor. Flagged for
// Phase 4 build-validation fixup.

#include "rtx_fork_hooks.h"
#include "rtx_fork_material_replace.h"  // [nfs2se 0041]

#include "rtx_asset_replacer.h"   // AssetReplacer, AssetReplacement
#include "rtx_options.h"          // RtxOptions::*, fast_unordered_set, InstanceCategories
#include "rtx_scene_manager.h"    // SceneManager, DrawCallMetaInfo

#include "dxvk_device.h"          // DxvkDevice::getCommon()->getResources()

#include <optional>
#include <unordered_map>

namespace dxvk {
namespace fork_hooks {

  // ---------------------------------------------------------------------------
  // externalDrawMeshReplacement
  //
  // Checks for a USD mesh/light replacement keyed on the API mesh handle hash,
  // same as the D3D9 draw-call path. Returns the replacement vector pointer if
  // one is found (caller must call determineMaterialData + drawReplacements and
  // then return), or null if no replacement exists.
  // ---------------------------------------------------------------------------
  std::vector<AssetReplacement>* externalDrawMeshReplacement(
      AssetReplacer& replacer, XXH64_hash_t meshHash) {
    // Check for mesh/light replacements keyed on the API mesh handle, same as
    // the D3D9 draw-call path. This lets .usd replacements target API-submitted
    // meshes (e.g. hash the remix API mesh handle and author a replacement).
    return replacer.getReplacementsForMesh(meshHash);
  }

  // ---------------------------------------------------------------------------
  // [nfs2se 0041] Material replacement of an API draw by its LEGACY texture hash.
  //
  // A D3D9 draw takes the mod material `mat_<hash>` named by its colour texture's image hash and the categories of every
  // rtx.*Textures list by the same hash. An API draw has no legacy material: before this patch it looked a mod material
  // up by the API material's own hash (a hash of constants, which no mod states) and took 13 of the 24 category lists by
  // the image hash of the albedo texture AFTER a replacement, so a replaced texture never reached its lists.
  //
  // Everything below runs on the command stream (remixapi_CreateMaterial / remixapi_DestroyMaterial emit their work there,
  // and so does the draw), so none of the tables has a lock.
  // ---------------------------------------------------------------------------
  namespace {
    using fork_material_replace::Request;

    struct MergedMaterial {
      const MaterialData* replacement = nullptr;  // the mod material the copy was made from (a mod reload changes it)
      std::optional<MaterialData> data;
    };

    std::unordered_map<remixapi_MaterialHandle, Request> s_replaceRequests;
    std::unordered_map<remixapi_MaterialHandle, MergedMaterial> s_mergedMaterials;
    fast_unordered_set s_loggedReplaced;
    fast_unordered_set s_loggedMissing;

    template<typename T>
    void copySampler(T& dst, uint8_t filter, uint8_t wrapU, uint8_t wrapV) {
      dst.getFilterMode() = filter;
      dst.getWrapModeU() = wrapU;
      dst.getWrapModeV() = wrapV;
    }

    // What determineMaterialData does for a D3D9 draw (mergeLegacyMaterial): the game's albedo texture and sampler fill
    // what the mod material does not state. The API material plays the game's part.
    void mergeApiMaterial(MaterialData& merged, const MaterialData& api) {
      uint8_t filter = lss::Mdl::Filter::Linear, wrapU = lss::Mdl::WrapMode::Repeat, wrapV = lss::Mdl::WrapMode::Repeat;
      TextureRef albedo;
      if (api.getType() == MaterialDataType::Opaque) {
        const auto& o = api.getOpaqueMaterialData();
        filter = o.getFilterMode(); wrapU = o.getWrapModeU(); wrapV = o.getWrapModeV();
        albedo = o.getAlbedoOpacityTexture();
      } else if (api.getType() == MaterialDataType::Translucent) {
        const auto& t = api.getTranslucentMaterialData();
        filter = t.getFilterMode(); wrapU = t.getWrapModeU(); wrapV = t.getWrapModeV();
      }
      if (merged.getType() == MaterialDataType::Opaque) {
        OpaqueMaterialData tmp;
        tmp.getAlbedoOpacityTexture() = albedo;
        copySampler(tmp, filter, wrapU, wrapV);
        merged.getOpaqueMaterialData().merge(tmp);
      } else if (merged.getType() == MaterialDataType::Translucent) {
        TranslucentMaterialData tmp;
        copySampler(tmp, filter, wrapU, wrapV);
        merged.getTranslucentMaterialData().merge(tmp);
      }
    }

    // The flags of the Replace EXT: this material's roughness / metallic win over the mod's (constant and map).
    void keepApiConstants(MaterialData& merged, const MaterialData& api, uint32_t flags) {
      if (merged.getType() != MaterialDataType::Opaque || api.getType() != MaterialDataType::Opaque) {
        return;
      }
      auto& m = merged.getOpaqueMaterialData();
      const auto& a = api.getOpaqueMaterialData();
      if (flags & fork_material_replace::kKeepRoughness) {
        m.setRoughnessConstant(a.getRoughnessConstant());
        m.setRoughnessTexture(a.getRoughnessTexture());
      }
      if (flags & fork_material_replace::kKeepMetallic) {
        m.setMetallicConstant(a.getMetallicConstant());
        m.setMetallicTexture(a.getMetallicTexture());
      }
    }

    void logReplaced(XXH64_hash_t hash, const MaterialData& m, uint32_t flags, bool explicitHash) {
      if (!s_loggedReplaced.insert(hash).second) {
        return;
      }
      if (m.getType() == MaterialDataType::Opaque) {
        const auto& o = m.getOpaqueMaterialData();
        Logger::info(str::format("[nfs2se matrepl] #", s_loggedReplaced.size(), " 0x", std::hex, hash, std::dec,
          " replaced (", explicitHash ? "explicit" : "albedo", " hash): opaque albedo=", o.getAlbedoOpacityTexture().isValid() ? 1 : 0,
          " normal=", o.getNormalTexture().isValid() ? 1 : 0, " rough=", o.getRoughnessTexture().isValid() ? 1 : 0,
          " metal=", o.getMetallicTexture().isValid() ? 1 : 0, " height=", o.getHeightTexture().isValid() ? 1 : 0,
          " emissive=", o.getEmissiveColorTexture().isValid() ? 1 : 0, " roughness=", o.getRoughnessConstant(),
          " metallic=", o.getMetallicConstant(), " intensity=", o.getEnableEmission() ? o.getEmissiveIntensity() : 0.f,
          " legacy_alpha=", o.getUseLegacyAlphaState() ? 1 : 0, " blend=", o.getBlendEnabled() ? 1 : 0,
          " alpha_test=", static_cast<int>(o.getAlphaTestType()), " keep=", flags));
      } else {
        Logger::info(str::format("[nfs2se matrepl] #", s_loggedReplaced.size(), " 0x", std::hex, hash, std::dec,
          " replaced (", explicitHash ? "explicit" : "albedo", " hash): ", m.getType() == MaterialDataType::Translucent ? "translucent" : "portal", " keep=", flags));
      }
    }
  } // anonymous namespace

  void externalMaterialReplaceRegister(remixapi_MaterialHandle handle, uint64_t legacyHash, uint32_t flags) {
    s_replaceRequests[handle] = Request { legacyHash, flags };
    s_mergedMaterials.erase(handle);
  }

  void externalMaterialReplaceForget(remixapi_MaterialHandle handle) {
    s_replaceRequests.erase(handle);
    s_mergedMaterials.erase(handle);
  }

  const MaterialData* externalDrawResolveMaterial(
      AssetReplacer& replacer, const MaterialData* material, remixapi_MaterialHandle handle,
      XXH64_hash_t& legacyHash) {
    const auto reqIt = s_replaceRequests.find(handle);
    const Request* req = reqIt != s_replaceRequests.end() ? &reqIt->second : nullptr;
    XXH64_hash_t albedoHash = 0;
    if (material->getType() == MaterialDataType::Opaque && material->getOpaqueMaterialData().getAlbedoOpacityTexture().isValid()) {
      albedoHash = material->getOpaqueMaterialData().getAlbedoOpacityTexture().getImageHash();
    }
    // The API material's own hash first (the stock behaviour of this path: a mod that names an API material's hash).
    if (MaterialData* pOwn = replacer.getReplacementMaterial(material->getHash())) {
      legacyHash = albedoHash;
      return pOwn;
    }
    const fork_material_replace::Plan plan = fork_material_replace::plan(RtxOptions::capiMaterialReplacement(), req, albedoHash);
    legacyHash = plan.hash;
    if (!plan.replace) {
      return material;
    }

    MaterialData* pReplacement = replacer.getReplacementMaterial(plan.hash);
    if (pReplacement == nullptr) {
      if (plan.explicitHash && s_loggedMissing.insert(plan.hash).second) {
        Logger::warn(str::format("[nfs2se matrepl] WARNING 0x", std::hex, plan.hash, std::dec,
          ": an API material asked for the mod material of this hash and no mod states mat_<hash> (is the mod installed and loaded?)"));
      }
      return material;
    }

    MergedMaterial& entry = s_mergedMaterials[handle];
    if (entry.replacement != pReplacement || !entry.data.has_value()) {
      entry.replacement = pReplacement;
      entry.data = *pReplacement;
      mergeApiMaterial(*entry.data, *material);
      keepApiConstants(*entry.data, *material, req ? req->flags : 0);
      logReplaced(plan.hash, *entry.data, req ? req->flags : 0, plan.explicitHash);
    }
    return &entry.data.value();
  }

  // ---------------------------------------------------------------------------
  // externalDrawTextureCategories
  //
  // [nfs2se 0041] The instance categories of a draw by its LEGACY texture hash, from every list DrawCallState::
  // setupCategoriesForTexture reads for a D3D9 draw (rtx_types.cpp, the tag) -- the same 24, so a conf line or a mod's rtx.conf
  // that tags a texture tags an API draw of it too. (Before: 13 of them, by the albedo image hash after a replacement.)
  // The D3D9 draw's "|| isUsingRaytracedRenderTarget" on IgnoreOpacityMicromap is a render-target draw; an API draw has none.
  // ---------------------------------------------------------------------------
  void externalDrawTextureCategories(
      XXH64_hash_t legacyHash,
      DrawCallState& drawCall) {
    if (legacyHash == 0 || legacyHash == kEmptyHash) {
      return;
    }
    auto applyCategory = [&](const fast_unordered_set& hashSet, InstanceCategories cat) {
      if (hashSet.find(legacyHash) != hashSet.end()) {
        drawCall.setCategory(cat, true);
      }
    };

    applyCategory(RtxOptions::worldSpaceUiTextures(), InstanceCategories::WorldUI);
    applyCategory(RtxOptions::worldSpaceUiBackgroundTextures(), InstanceCategories::WorldMatte);
    applyCategory(RtxOptions::ignoreTextures(), InstanceCategories::Ignore);
    applyCategory(RtxOptions::ignoreLights(), InstanceCategories::IgnoreLights);
    applyCategory(RtxOptions::antiCullingTextures(), InstanceCategories::IgnoreAntiCulling);
    applyCategory(RtxOptions::motionBlurMaskOutTextures(), InstanceCategories::IgnoreMotionBlur);
    applyCategory(RtxOptions::opacityMicromapIgnoreTextures(), InstanceCategories::IgnoreOpacityMicromap);
    applyCategory(RtxOptions::ignoreAlphaOnTextures(), InstanceCategories::IgnoreAlphaChannel);
    applyCategory(RtxOptions::ignoreBakedLightingTextures(), InstanceCategories::IgnoreBakedLighting);
    applyCategory(RtxOptions::hideInstanceTextures(), InstanceCategories::Hidden);
    applyCategory(RtxOptions::particleTextures(), InstanceCategories::Particle);
    applyCategory(RtxOptions::beamTextures(), InstanceCategories::Beam);
    applyCategory(RtxOptions::ignoreTransparencyLayerTextures(), InstanceCategories::IgnoreTransparencyLayer);
    applyCategory(RtxOptions::decalTextures(), InstanceCategories::DecalStatic);
    applyCategory(RtxOptions::dynamicDecalTextures(), InstanceCategories::DecalDynamic);
    applyCategory(RtxOptions::singleOffsetDecalTextures(), InstanceCategories::DecalSingleOffset);
    applyCategory(RtxOptions::nonOffsetDecalTextures(), InstanceCategories::DecalNoOffset);
    applyCategory(RtxOptions::animatedWaterTextures(), InstanceCategories::AnimatedWater);
    applyCategory(RtxOptions::playerModelTextures(), InstanceCategories::ThirdPersonPlayerModel);
    applyCategory(RtxOptions::playerModelBodyTextures(), InstanceCategories::ThirdPersonPlayerBody);
    applyCategory(RtxOptions::terrainTextures(), InstanceCategories::Terrain);
    applyCategory(RtxOptions::skyBoxTextures(), InstanceCategories::Sky);
    applyCategory(RtxOptions::particleEmitterTextures(), InstanceCategories::ParticleEmitter);
    applyCategory(RtxOptions::hairCardTextures(), InstanceCategories::HairCards);
  }

  // ---------------------------------------------------------------------------
  // externalDrawObjectPicking
  //
  // Stores per-draw texture hash metadata in SceneManager::m_drawCallMeta when
  // object picking is active, mirroring the D3D9 draw path which populates
  // m_drawCallMeta in processDrawCallState. API draws supply their own
  // drawCallID via remixapi_InstanceInfoObjectPickingEXT, so we store it here.
  //
  // ACCESS NOTE: this function uses SceneManager::m_drawCallMeta (private) and
  // SceneManager::DrawCallMetaInfo (private nested type). A friend declaration
  // for this function is required in SceneManager, or the member / type must be
  // made accessible via a public helper. See file-level comment above.
  // ---------------------------------------------------------------------------
  void externalDrawObjectPicking(
      DxvkDevice& device,
      DrawCallState& drawCall,
      XXH64_hash_t textureHash,
      SceneManager& scene) {
    // Store texture hash metadata for object picking (mirrors the D3D9 draw
    // path which populates m_drawCallMeta in processDrawCallState). API draws
    // supply their own drawCallID via remixapi_InstanceInfoObjectPickingEXT,
    // so we hash it in directly here.
    const bool objectPickingActive = device.getCommon()->getResources().getRaytracingOutput()
      .m_primaryObjectPicking.isValid();
    if (objectPickingActive && drawCall.drawCallID != 0 &&
        textureHash != 0 && textureHash != kEmptyHash) {
      auto meta = SceneManager::DrawCallMetaInfo {};
      meta.legacyTextureHash = textureHash;

      std::lock_guard lock { scene.m_drawCallMeta.mutex };
      auto [iter, isNew] = scene.m_drawCallMeta.infos[scene.m_drawCallMeta.ticker].emplace(drawCall.drawCallID, meta);
      ONCE_IF_FALSE(isNew, Logger::warn(
        "Found multiple API draw calls with the same \'objectPickingValue\'. "
        "Some objects might not be available through object picking"));
    }
  }

} // namespace fork_hooks
} // namespace dxvk

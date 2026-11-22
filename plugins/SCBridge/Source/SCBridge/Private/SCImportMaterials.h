// FSCImportMaterials: per-import material phase. Describes every (sidecar, palette) pair used by the scene,
// decodes the referenced textures once (worker), then creates textures + MIDs owned by the vehicle (game thread).
#pragma once
#include "CoreMinimal.h"
#include "SCMaterialBuilder.h"

class ASCVehicle;
class UMaterialInterface;
struct FSCTextureCacheRequest;

class FSCImportMaterials
{
public:
    /** Pure: build descriptions for every instance's sidecar/palette pair (engine-glow targets applied). */
    void Describe(const FSCSceneManifest& Manifest);
    /** Worker: decode the deduplicated texture set and resolve the descriptions' texture means. */
    void Decode(const FSCPackageSource& Source, const FSCTextureCacheRequest* Cache = nullptr);
    /** Game thread: textures and material instances, retained by the vehicle. */
    void Instantiate(ASCVehicle* Vehicle, TArray<FString>& Warnings);
    /** Material for a mesh slot of an instance, or null to keep the mesh's placeholder. Host-tinted decals
     *  get one instance per host palette channel (created on first use). */
    UMaterialInterface* Find(const FString& Sidecar, const FString& PaletteId, int32 Ordinal, const FString& HostChannel = FString());
    /** Dominant host paint channel of a mesh slot set, weighted by triangles (add-on rule; priority order on ties). */
    static FString HostChannel(const FSCMaterialSet* Set, const TArray<FString>& SourceNames, const TArray<int32>& SlotTriangles);
    /** WP-I1: the instance for a screen slot (M_SC_Screen) showing `Image` (a package path from the instance's
     *  ui_bindings, FSCSceneInstance::ScreenSlotImage): one instance per (sidecar, submaterial, image), shared by every
     *  panel with that image, emitting at the authored glow (FSCMaterialDesc::ScreenGlow; Find's unbound instance emits
     *  nothing). Null when the slot is not a screen or the image did not load (the slot keeps Find's instance). */
    UMaterialInterface* FindScreen(const FString& Sidecar, const FString& PaletteId, int32 Ordinal, const FString& Image);
    /** WP-P5 floor panel: a new instance of a screen slot's material that shows Image (any texture, a viewer-drawn render
     *  target) and emits at the authored glow, like FindScreen's; null when the slot is not a screen. Owned by the caller
     *  (outer: the owning vehicle; not added to its Materials). */
    UMaterialInstanceDynamic* CreateScreen(const FString& Sidecar, const FString& PaletteId, int32 Ordinal, UTexture* Image);
    /** A decoded texture of this import by package path and role (null when it was not requested or failed). */
    UTexture2D* Texture(const FString& Path, ESCTextureRole Role = ESCTextureRole::Color) const { return Textures.FindRef(FSCTextureKey{Path, Role}); }
    /** Whether a submaterial is drawn by a screen master (M_SC_Screen / _Translucent). */
    bool IsScreen(const FString& Sidecar, const FString& PaletteId, int32 Ordinal) const;
    /** Draws nothing (zero-emission additive); for split sections that have no authored instance. */
    UMaterialInterface* Invisible();
    /** Another actor assembled from the same import (FSCImportJob from retained assets): instances created on demand
     *  from now on (host-tinted variants, the invisible material) belong to it. */
    void Rebind(ASCVehicle* NewOwner) { Owner = NewOwner; }

    /** Sidecar submaterial for a GLB slot: array ordinal per contract; older compact GLBs carry an explicit
     *  source_name that wins only when the ordinal's name disagrees. */
    static const FSCSubmaterial* Resolve(const FSCMaterialSet* Set, const TArray<FString>& SourceNames, int32 Slot);

    FSCTextureStats TextureStats;
    int32 MaterialCount = 0, SetCount = 0, EngineGlowCount = 0;
    double DescribeSeconds = 0, MaterialSeconds = 0;
    TMap<ESCMaster, int32> MasterCounts;
    /** WP-I1 panel images: the distinct images the scene's operating panels may show (decoded with the material
     *  textures); the bindings with a render left unbound by the owner's decision (vehicle screens, displays), and those
     *  of an export without source_item_type (unclassified, left dark). */
    TSet<FString> PanelImages;
    int32 SkippedVehicleScreens = 0, UnclassifiedBindings = 0;

private:
    struct FSet
    {
        FString Sidecar;
        TArray<FSCMaterialDesc> Descs;
        TArray<bool> EngineGlow;
        TArray<UMaterialInterface*> Instances;
        const FSCPalette* Palette = nullptr; // manifest-owned; valid for the import call
        TMap<TPair<int32, FString>, UMaterialInterface*> HostVariants;
    };
    int32 SetIndex(const FString& Sidecar, const FString& PaletteId) const;
    ASCVehicle* Owner = nullptr;
    UMaterialInterface* InvisibleMaterial = nullptr;
    TMap<FSCTextureKey, UTexture2D*> Textures;
    TMap<TTuple<FString, int32, FString>, UMaterialInterface*> ScreenInstances; // (sidecar, submaterial, image)
    TArray<FSet> Sets;
    TMap<FString, TMap<FString, int32>> SetBySidecarAndPalette;
    TArray<FSCTextureKey> Keys;
    TSet<FSCTextureKey> SampledKeys, MeanKeys; // bound to a material / read for its mean colour (may be both)
    TArray<FSCDecodedTexture> Decoded;
};

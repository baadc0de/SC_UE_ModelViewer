// FSCImportMaterials: per-import material phase. Describes every (sidecar, palette) pair used by the scene,
// decodes the referenced textures once (worker), then creates textures + MIDs owned by the vehicle (game thread).
#pragma once
#include "CoreMinimal.h"
#include "SCMaterialBuilder.h"

class ASCVehicle;
class UMaterialInterface;

class FSCImportMaterials
{
public:
    /** Pure: build descriptions for every instance's sidecar/palette pair (engine-glow targets applied). */
    void Describe(const FSCSceneManifest& Manifest);
    /** Worker: decode the deduplicated texture set. */
    void Decode(const FSCPackageSource& Source);
    /** Game thread: textures and material instances, retained by the vehicle. */
    void Instantiate(ASCVehicle* Vehicle, TArray<FString>& Warnings);
    /** Material for a mesh slot of an instance, or null to keep the mesh's placeholder. Host-tinted decals
     *  get one instance per host palette channel (created on first use). */
    UMaterialInterface* Find(const FString& Sidecar, const FString& PaletteId, int32 Ordinal, const FString& HostChannel = FString());
    /** Dominant host paint channel of a mesh slot set, weighted by triangles (add-on rule; priority order on ties). */
    static FString HostChannel(const FSCMaterialSet* Set, const TArray<FString>& SourceNames, const TArray<int32>& SlotTriangles);
    /** Draws nothing (zero-emission additive); for split sections that have no authored instance. */
    UMaterialInterface* Invisible();

    /** Sidecar submaterial for a GLB slot: array ordinal per contract; older compact GLBs carry an explicit
     *  source_name that wins only when the ordinal's name disagrees. */
    static const FSCSubmaterial* Resolve(const FSCMaterialSet* Set, const TArray<FString>& SourceNames, int32 Slot);

    FSCTextureStats TextureStats;
    int32 MaterialCount = 0, SetCount = 0, EngineGlowCount = 0;
    double DescribeSeconds = 0, MaterialSeconds = 0;
    TMap<ESCMaster, int32> MasterCounts;

private:
    struct FSet
    {
        FString Sidecar, PaletteId;
        TArray<FSCMaterialDesc> Descs;
        TArray<bool> EngineGlow;
        TArray<UMaterialInterface*> Instances;
        const FSCPalette* Palette = nullptr; // manifest-owned; valid for the import call
        TMap<TPair<int32, FString>, UMaterialInterface*> HostVariants;
    };
    ASCVehicle* Owner = nullptr;
    UMaterialInterface* InvisibleMaterial = nullptr;
    TMap<FSCTextureKey, UTexture2D*> Textures;
    TArray<FSet> Sets;
    TArray<FSCTextureKey> Keys;
    TArray<FSCDecodedTexture> Decoded;
};

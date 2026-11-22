// SCKeepAssets: rename transient import objects into /Game packages (references between them stay intact),
// convert dynamic material instances to constants, bake mesh slot materials from component overrides, save.
#include "SCKeepAssets.h"
#include "SCVehicle.h"
#include "SCClipCatalog.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCKeep, Log, All);

namespace
{
FString SafeName(FString Name)
{
    for (TCHAR& C : Name)
        if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return Name.IsEmpty() ? TEXT("Object") : Name;
}

struct FKeeper
{
    FString Root;
    TArray<UPackage*> Packages;
    TMap<UObject*, UObject*> Kept; // transient source -> asset (identity for renamed objects)
    TArray<FString> Warnings;

    /** Move Object (and its subobjects) into its own package under Root/Folder, public + standalone. */
    UObject* Move(UObject* Object, const FString& Folder, const FString& PreferredName = FString())
    {
        if (!Object) return nullptr;
        if (UObject** Found = Kept.Find(Object)) return *Found;
        const FString Base = SafeName(PreferredName.IsEmpty() ? Object->GetName() : PreferredName);
        FString Name = Base;
        for (int32 Suffix = 2; FindPackage(nullptr, *(Root / Folder / Name)) || FPackageName::DoesPackageExist(Root / Folder / Name); ++Suffix)
            Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
        UPackage* Package = CreatePackage(*(Root / Folder / Name));
        Package->FullyLoad();
        Object->Rename(*Name, Package, REN_DontCreateRedirectors | REN_NonTransactional);
        Object->ClearFlags(RF_Transient);
        Object->SetFlags(RF_Public | RF_Standalone);
        ForEachObjectWithOuter(Object, [](UObject* Inner) { Inner->ClearFlags(RF_Transient); }); // nested by default
        Package->MarkPackageDirty();
        Packages.Add(Package);
        Kept.Add(Object, Object);
        FAssetRegistryModule::AssetCreated(Object);
        return Object;
    }
    /** Runtime textures carry only platform mips; give the asset an editable source from mip 0. */
    void AddTextureSource(UTexture2D* Texture)
    {
        FTexturePlatformData* Platform = Texture->GetPlatformData();
        if (!Platform || Platform->Mips.IsEmpty() || Platform->PixelFormat != PF_B8G8R8A8)
        {
            Warnings.Add(Texture->GetName() + TEXT(": no BGRA8 mip data; saved without source"));
            return;
        }
        FTexture2DMipMap& Mip = Platform->Mips[0];
        const int64 Expected = int64(Mip.SizeX) * Mip.SizeY * 4;
        const void* Data = Mip.BulkData.LockReadOnly();
        if (Data && Mip.BulkData.GetBulkDataSize() == Expected)
            Texture->Source.Init(Mip.SizeX, Mip.SizeY, 1, 1, TSF_BGRA8, static_cast<const uint8*>(Data));
        else
            Warnings.Add(Texture->GetName() + TEXT(": mip 0 not CPU-resident; saved without source"));
        Mip.BulkData.Unlock();
    }
    /** Constant copy of a dynamic instance: same parent, every overridden parameter. */
    UMaterialInterface* Constant(UMaterialInterface* Material)
    {
        auto* MID = Cast<UMaterialInstanceDynamic>(Material);
        if (!MID) return Material;
        if (UObject** Found = Kept.Find(MID)) return Cast<UMaterialInterface>(*Found);
        const FString Base = SafeName(TEXT("MI_") + MID->GetName());
        FString Name = Base;
        for (int32 Suffix = 2; FindPackage(nullptr, *(Root / TEXT("Materials") / Name)); ++Suffix)
            Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
        UPackage* Package = CreatePackage(*(Root / TEXT("Materials") / Name));
        auto* MIC = NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone);
        MIC->SetParentEditorOnly(MID->Parent);
        for (const FScalarParameterValue& P : MID->ScalarParameterValues)
            MIC->SetScalarParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        for (const FVectorParameterValue& P : MID->VectorParameterValues)
            MIC->SetVectorParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        for (const FTextureParameterValue& P : MID->TextureParameterValues)
            MIC->SetTextureParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        MIC->PostEditChange();
        Package->MarkPackageDirty();
        Packages.Add(Package);
        Kept.Add(MID, MIC);
        FAssetRegistryModule::AssetCreated(MIC);
        return MIC;
    }
};
} // namespace

FSCKeepResult SCKeepAssets::Keep(ASCVehicle* Vehicle, const FString& Entity)
{
    FSCKeepResult Result;
    if (!Vehicle)
    {
        Result.Message = TEXT("Nothing loaded");
        return Result;
    }
    const double Start = FPlatformTime::Seconds();
    FKeeper K;
    const FString Base = TEXT("/Game/SC/") + SafeName(Entity);
    K.Root = Base;
    for (int32 Suffix = 2; FPackageName::DoesPackageExist(K.Root / TEXT("Clips")) || FindPackage(nullptr, *(K.Root / TEXT("Clips"))); ++Suffix)
        K.Root = FString::Printf(TEXT("%s_%d"), *Base, Suffix); // never overwrite an earlier Keep

    for (UTexture2D* Texture : Vehicle->Textures)
        if (Texture && !K.Kept.Contains(Texture))
        {
            K.AddTextureSource(Texture);
            K.Move(Texture, TEXT("Textures"));
        }
    for (auto& Pair : Vehicle->GoboTextures)
        if (Pair.Value) K.Move(Pair.Value, TEXT("Textures"));
    // Slot materials: the first component override of each mesh slot becomes the asset's material.
    TArray<UStaticMeshComponent*> MeshComponents;
    Vehicle->GetComponents<UStaticMeshComponent>(MeshComponents);
    for (UStaticMeshComponent* C : MeshComponents)
    {
        UStaticMesh* Mesh = C->GetStaticMesh();
        if (!Mesh || !Vehicle->Meshes.Contains(Mesh)) continue;
        for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
            if (UMaterialInterface* Override = C->OverrideMaterials.IsValidIndex(Slot) ? C->OverrideMaterials[Slot].Get() : nullptr)
                if (!K.Kept.Contains(Mesh)) Mesh->GetStaticMaterials()[Slot].MaterialInterface = K.Constant(Override);
    }
    for (UStaticMesh* Mesh : Vehicle->Meshes)
        K.Move(Mesh, TEXT("Meshes"));
    // Runtime skeletal meshes carry render data only (FSCSkeletalMeshImporter builds no editor source model), and
    // the editor's PostLoad requires one: keep their skeletons and clips, skip the meshes with a warning.
    // TODO(WP-U3): build the editor ImportedModel for soft-skinned parts so Keep can persist rigs.
    for (USkeletalMesh* Mesh : Vehicle->SkeletalMeshes)
        if (Mesh)
        {
            K.Move(Mesh->GetSkeleton(), TEXT("Rigs"));
            K.Warnings.Add(Mesh->GetName() + TEXT(": skeletal mesh not kept (no editor source model)"));
        }
    if (USCClipCatalog* Catalog = Vehicle->ClipCatalog)
    {
        for (FSCImportedClip& Clip : Catalog->Clips)
        {
            const FString ClipName = FPaths::GetBaseFilename(Clip.Name);
            K.Move(Clip.Sequence, TEXT("Animations"), ClipName);
            for (FSCClipBinding& Binding : Clip.Bindings)
                for (TObjectPtr<UAnimSequence>& Sequence : Binding.Sequences)
                    K.Move(Sequence, TEXT("Animations"), ClipName);
        }
        K.Move(Catalog, FString(), TEXT("Clips"));
    }

    int32 Saved = 0;
    for (UPackage* Package : K.Packages)
    {
        UObject* Asset = Package->FindAssetInPackage();
        const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        if (UPackage::SavePackage(Package, Asset, *File, Args))
            ++Saved;
        else
            K.Warnings.Add(TEXT("save failed: ") + Package->GetName());
    }
    for (const FString& W : K.Warnings)
        UE_LOG(LogSCKeep, Warning, TEXT("KEEP_WARNING %s"), *W);
    UE_LOG(LogSCKeep, Display, TEXT("KEEP root=%s packages=%d saved=%d warnings=%d seconds=%.3f"), *K.Root, K.Packages.Num(), Saved,
           K.Warnings.Num(), FPlatformTime::Seconds() - Start);
    Result.Root = K.Root;
    Result.Saved = Saved;
    Result.bOK = Saved == K.Packages.Num() && Saved > 0;
    Result.Message = FString::Printf(TEXT("Kept %d assets under %s%s"), Saved, *K.Root,
                                     K.Warnings.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%d warnings, see log)"), K.Warnings.Num()));
    return Result;
}
